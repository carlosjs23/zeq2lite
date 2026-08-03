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

// A mode brings its own content. The tag vocabulary is shared - a tag earned in
// training is the same bit in the tournament, which is what lets the Budokai
// gate on it - but the rules are not: the solo arc talks to a player standing
// in an empty map, and half of it would be firing over a fight.
#define BUDOKAI_RULES_FILE	"rules/budokai.rules"

// The tag a fighter needs to enter the tournament queue, and the world tag the
// round state raises. Both are ordinary declarations in rules/tags.def; the C
// looks them up by name and gates on nothing when the content omits them.
#define BUDOKAI_ENTRY_TAG	"budokai.entry"
#define BUDOKAI_ROUND_TAG	"world.round.inProgress"

// One ring-out per round, and the round ends on the first one. The cooldown is
// belt and braces for the frames between the decision and the intermission
// actually starting, where both fighters are still being run.
#define BUDOKAI_RINGOUT_COOLDOWN	3000

// Tags under this prefix belong to the shared world set rather than to the
// player who tripped the rule. The prefix is the only marker the language has,
// and it is the one every world tag in the plan already carries.
#define TRAINING_WORLD_PREFIX	"world."

// Untrained players keep the first transformation, so shipped content that
// unlocks tiers 2 and 3 is a ladder rather than a dead end.
#define TRAINING_BASE_TIER	1

static qboolean	trainingLive;
static char	trainingMapName[MAX_QPATH];

// Budokai state. Statics rather than level_locals_t fields because a tournament
// round ends in map_restart, which re-runs G_InitGame and therefore clears these
// exactly when the round they describe is over.
static int	budokaiEntryTag = -1;
static int	budokaiRoundTag = -1;
static int	budokaiRoundStart;
static int	budokaiRingOutTime;

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
// `masterNear is rhogan` is validated against - installing them afterwards would
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

// rules/arena_<mapname>.def, derived the same way the master placements are.
static const char *arenaFile(void){
	return va("rules/arena_%s.def",trainingMapName);
}

// The mode picks its content file. GT_TOURNAMENT is the Budokai, and its file is
// required once it is the mode being played: falling back to the training arc
// would put a master's lesson in the middle of a title fight, which is a louder
// failure than refusing to start.
static const char *contentFile(void){
	return g_gametype.integer == GT_TOURNAMENT ? BUDOKAI_RULES_FILE : TRAINING_RULES_FILE;
}

void G_TrainingInit(void){
	trainingLive = qfalse;
	budokaiEntryTag = -1;
	budokaiRoundTag = -1;
	budokaiRoundStart = 0;
	budokaiRingOutTime = 0;
	G_RulesReset();
	G_RingReset();
	trap_Cvar_VariableStringBuffer("mapname",trainingMapName,sizeof(trainingMapName));
	if(!g_training.integer){
		G_Printf("Training mode: off (g_training 0).\n");
		return;
	}
	loadMasters();
	// The ring is loaded in every gametype, not just the tournament, because
	// arenaplace is authored from wherever the author is standing and reloading
	// what is already on disk is what makes the editor's round trip visible.
	if(!G_RingLoad(arenaFile())){
		G_Printf("Training mode: %s did not load, no ring is defined.\n",arenaFile());
		G_RingReset();
	}
	if(!G_RulesLoad(TRAINING_TAGS_FILE,contentFile())){
		// The error text is the product here, so it is printed whole and the
		// mode is switched off. A content typo must not take a server down;
		// ruletest and the Criterion suite are where it stays a hard failure.
		G_Printf("Training mode: disabled, content did not load.\n");
		G_Printf("%s\n",G_RulesError());
		G_RulesReset();
		return;
	}
	trainingLive = qtrue;
	// Resolved once, here, so the per-frame paths never look a tag up by name.
	// Content that declares neither gates nobody and raises nothing, which is
	// what keeps the training arc's own tags.def working unchanged.
	budokaiEntryTag = G_TagFind(BUDOKAI_ENTRY_TAG);
	budokaiRoundTag = G_TagFind(BUDOKAI_ROUND_TAG);
	G_Printf("Training mode: %i rule(s), %i test vector(s), %i tag(s) declared, from %s.\n",
		G_RulesCount(),G_RulesTestCount(),G_TagCount(),contentFile());
	if(G_RingDefined()){
		const ring_t *r = G_RingGet();
		G_Printf("Training mode: ring at %.0f %.0f, radius %.0f, floor %.0f (%s).\n",
			r->center[0],r->center[1],r->radius,r->floor,arenaFile());
	}
}

// ----------------------------------------------------------------- events
//
// Every training event that changes what a player owns goes to games.log as one
// line, in the shape the log already speaks - a label, a colon, then fields
// (`ClientConnect: 3`). Fields are positional and the arity is fixed per event,
// so a line parses by splitting on whitespace and the only quoted field is the
// last one:
//
//   Training: <clientNum> <key> say "<text>"
//   Training: <clientNum> <key> grant <tagName>
//   Training: <clientNum> <key> remove <tagName>
//   Training: <clientNum> <key> unlock-tier <tier>
//   Training: <clientNum> <key> objective-assigned <id> <trackFact> <goal> "<text>"
//   Training: <clientNum> <key> objective-complete <id>
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
static void loadProgress(int clientNum){
	gclient_t *client;
	progress_t p;
	progressLoad_t report;
	int i;

	client = level.clients + clientNum;
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

void G_TrainingClientBegin(int clientNum){
	gclient_t *client;

	client = level.clients + clientNum;
	if(!trainingLive){return;}
	loadProgress(clientNum);
	// The Budokai entry gate cannot live in G_InitSessionData, which runs at
	// connect - before this function has read the player's tags off the disk.
	// Here they are loaded and ClientSpawn has not picked a spawn point yet, so
	// an untrained challenger is seated before he ever stands in the ring.
	if(client->sess.sessionTeam != TEAM_SPECTATOR && !G_TrainingMayFight(client)){
		client->sess.sessionTeam = TEAM_SPECTATOR;
		client->sess.spectatorState = SPECTATOR_FREE;
		AddTournamentQueue(client);
		G_TrainingEntryRefused(clientNum);
	}
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
	// The ring is the second trigger volume with no entity behind it, and both
	// facts are zero on a map with no ring - so content that keys on being
	// outside it stays silent rather than firing everywhere.
	facts[fRingDistance] = (int)G_RingDistance(ps->origin);
	facts[fRingHeight] = (int)G_RingHeight(ps->origin);
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
		trainingEvent(clientNum,va("grant %s",G_TagName(action->tag)));
		break;
	case acRemove:
		if(!Q_strncmp(G_TagName(action->tag),TRAINING_WORLD_PREFIX,(int)strlen(TRAINING_WORLD_PREFIX))){
			G_TagClear(&g_worldTags,action->tag);
			break;
		}
		G_TagClear(&client->pers.tags,action->tag);
		client->pers.progressDirty = qtrue;
		trainingEvent(clientNum,va("remove %s",G_TagName(action->tag)));
		break;
	case acSay:
		// One delivery to the player. This also went out as a console print
		// while the HUD had nothing to draw it with; now that it does, the print
		// was a second copy of every line in the chat feed. The log line is not
		// that print: games.log is the event seam, a spoken line is an event,
		// and a mode whose whole voice is authored content is otherwise a mode
		// nothing can be verified about after the fact.
		trainingToast(clientNum,action->text);
		trainingEvent(clientNum,va("say \"%s\"",action->text));
		break;
	case acObjective:
		Q_strncpyz(client->pers.objectiveText,action->text,sizeof(client->pers.objectiveText));
		client->pers.objectiveTrack = action->track;
		client->pers.objectiveGoal = action->value;
		client->pers.objectiveId = G_RulesIndexOf(rule) + 1;
		client->pers.objectiveComplete = qfalse;
		trap_SendServerCommand(clientNum,va("trobj \"%s\" %i %i %i",action->text,
			client->pers.objectiveId,action->track,action->value));
		trainingEvent(clientNum,va("objective-assigned %i %i %i \"%s\"",
			client->pers.objectiveId,action->track,action->value,action->text));
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
			trainingEvent(clientNum,va("unlock-tier %i",action->value));
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
		trainingEvent(ent - g_entities,va("objective-complete %i",client->pers.objectiveId));
	}
}

// ---------------------------------------------------------------- budokai
//
// The Tenkaichi Budokai is GT_TOURNAMENT with three things added: a ring, a
// round state the rule engine can read, and an entry requirement. None of them
// is a new mode - the bracket, the queue and the round restart are the stock
// tournament code, and everything here either feeds it facts or ends a round
// through the same path a kill already does.
//
// With g_training 0 none of it runs and the tournament is stock.

// Round state, driven from the tournament flow and read by content as the
// roundState world fact. Only GT_TOURNAMENT drives it: in the other gametypes
// the slot stays at its Phase 1 value of zero rather than acquiring a meaning
// the mode does not have.
static void budokaiRoundFrame(void){
	int state;

	if(g_gametype.integer != GT_TOURNAMENT){return;}
	if(level.intermissiontime || level.intermissionQueued){
		state = roundOver;
	}else if(level.numPlayingClients != 2 || level.warmupTime != 0){
		// warmupTime -1 is "waiting for players" and a positive value is the
		// countdown; only zero is a round actually being fought.
		state = roundWaiting;
	}else{
		state = roundInProgress;
	}
	if(state == roundInProgress && g_worldFacts[wRoundState] != roundInProgress){
		budokaiRoundStart = level.time;
		G_LogPrintf("Budokai: round start\n");
	}
	if(state != g_worldFacts[wRoundState]){
		G_LogPrintf("Budokai: roundState %i\n",state);
	}
	g_worldFacts[wRoundState] = state;
	if(state == roundInProgress){
		g_worldFacts[wRoundTime] = level.time - budokaiRoundStart;
	}else if(state == roundWaiting){
		g_worldFacts[wRoundTime] = 0;
	}
	// The world tag is the same state said in the vocabulary rules gate on, so a
	// rule can require it instead of restating the fact.
	if(state == roundInProgress){
		G_TagSet(&g_worldTags,budokaiRoundTag);
	}else{
		G_TagClear(&g_worldTags,budokaiRoundTag);
	}
}

void G_TrainingWorldFrame(void){
	if(!trainingLive){return;}
	budokaiRoundFrame();
}

// A ring-out ends the round exactly where a kill ends it: the surviving fighter
// takes the point, CalculateRanks sorts the loser to sortedClients[1], and
// LogExit queues the intermission that ExitLevel turns into
// RemoveTournamentLoser plus map_restart. The loser is never killed - being
// rung out is a defeat, not a death, and the source material is emphatic.
static void budokaiRingOut(gentity_t *ent){
	gclient_t *winner;
	int loserNum,winnerNum,award,i;

	loserNum = ent - g_entities;
	winnerNum = -1;
	for(i=0;i<level.maxclients;i++){
		if(i == loserNum){continue;}
		if(level.clients[i].pers.connected != CON_CONNECTED){continue;}
		if(level.clients[i].sess.sessionTeam == TEAM_SPECTATOR){continue;}
		winnerNum = i;
		break;
	}
	// Nobody to award the round to is not a round, so stepping off the edge
	// alone in an empty arena costs nothing.
	if(winnerNum < 0){return;}
	budokaiRingOutTime = level.time;
	winner = level.clients + winnerNum;
	// A ring-out decides the round whatever the score was, so the award is at
	// least one point clear of the loser rather than a bare increment - which
	// would otherwise hand RemoveTournamentLoser the wrong client when the
	// fighter who stepped out was ahead on kills.
	award = winner->ps.persistant[PERS_SCORE] + 1;
	if(award <= ent->client->ps.persistant[PERS_SCORE]){
		award = ent->client->ps.persistant[PERS_SCORE] + 1;
	}
	winner->ps.persistant[PERS_SCORE] = award;
	CalculateRanks();
	trap_SendServerCommand(-1,va("print \"%s" S_COLOR_WHITE " was rung out by %s" S_COLOR_WHITE ".\n\"",
		ent->client->pers.netname,winner->pers.netname));
	trap_SendServerCommand(-1,va("cp \"RING OUT!\n%s" S_COLOR_WHITE " takes the round.\n\"",
		winner->pers.netname));
	G_LogPrintf("Budokai: ringout %i winner %i\n",loserNum,winnerNum);
	LogExit("Ring out.");
}

// Grounded is read exactly as the fact pass reads airborne, jump and soar
// included: a fighter carried over the edge by a boost has not landed, and the
// whole point of the rule is that flying out is legal.
static void budokaiRingCheck(gentity_t *ent){
	playerState_t *ps;
	qboolean grounded;

	if(g_gametype.integer != GT_TOURNAMENT){return;}
	if(!G_RingDefined()){return;}
	if(g_worldFacts[wRoundState] != roundInProgress){return;}
	if(ent->client->sess.sessionTeam == TEAM_SPECTATOR){return;}
	if(budokaiRingOutTime && level.time - budokaiRingOutTime < BUDOKAI_RINGOUT_COOLDOWN){return;}
	ps = &ent->client->ps;
	// A body on the floor outside the ring is a fighter who already lost the
	// round; isDead is the flag the rest of the module reads for that.
	if(ps->bitFlags & isDead){return;}
	grounded = ((ps->bitFlags & (usingJump | usingSoar)) || ps->groundEntityNum == ENTITYNUM_NONE) ? qfalse : qtrue;
	if(!G_RingIsOut(ps->origin,grounded)){return;}
	budokaiRingOut(ent);
}

// The gate the plan's payoff rests on: train, then test it. A server whose
// content declares no entry tag gates nobody, so this costs nothing to a mode
// that does not want it.
//
// Bots bypass the gate, deliberately. budokai.entry is a progression gate on a
// human's own advancement, and an AI fighter is not advancing through anything:
// it was spawned by a player who has already passed the gate, as the opponent
// he passed it to fight. Gating the sparring partner would only mean a trained
// player standing alone in the ring.
//
// Losing is not bypassed. A bot that is rung out or beaten goes through
// RemoveTournamentLoser like a human - to the back of the queue, not out of the
// game - because a bot removed after one round would end the practice session
// the player came for, and because AddTournamentPlayer pulls it straight back
// in when it is the only one waiting, which is the next round.
qboolean G_TrainingMayFight(gclient_t *client){
	if(!trainingLive){return qtrue;}
	if(g_gametype.integer != GT_TOURNAMENT){return qtrue;}
	if(budokaiEntryTag < 0){return qtrue;}
	if(client->pers.isDummy){return qtrue;}
	return G_TagTest(&client->pers.tags,budokaiEntryTag);
}

void G_TrainingEntryRefused(int clientNum){
	if(!trainingLive){return;}
	trainingToast(clientNum,"Only trained fighters enter the Budokai.");
}

// Spectators watch from the ring rather than from the intermission point, which
// was authored to show off a map and not to show a fight. Nothing free-floating:
// this is one origin and one set of angles handed to the spawn selector.
// A fighter starts a round in the ring, on the side of it the other fighter is
// not on. The corner is picked by slot order rather than by score so that both
// clients agree without the two spawns having to be resolved together, and it
// only applies to the tournament: in every other gametype the map's own spawn
// points are still what the author placed.
qboolean G_TrainingFighterSpawn(gclient_t *client,vec3_t origin,vec3_t angles){
	static const vec3_t mins = {-15,-15,-24};
	static const vec3_t maxs = {15,15,32};
	trace_t trace;
	vec3_t start,end;
	int index,i,slot;

	if(!trainingLive){return qfalse;}
	if(g_gametype.integer != GT_TOURNAMENT){return qfalse;}
	if(!G_RingDefined()){return qfalse;}
	if(client->sess.sessionTeam == TEAM_SPECTATOR){return qfalse;}
	slot = client - level.clients;
	index = 0;
	for(i=0;i<slot;i++){
		if(level.clients[i].pers.connected == CON_DISCONNECTED){continue;}
		if(level.clients[i].sess.sessionTeam == TEAM_SPECTATOR){continue;}
		index++;
	}
	G_RingCorner(index,origin,angles);
	// The arena file states one floor height for the whole ring, and the terrain
	// under a corner of it need not be at that height - on desert it is a
	// thousand units lower, which would open every round with both fighters
	// falling. Settle onto what is actually there; keep the authored height if
	// there is nothing, because a ring over open air is the author's business.
	VectorCopy(origin,start);
	start[2] += 32;
	VectorCopy(origin,end);
	end[2] -= 4096;
	trap_Trace(&trace,start,mins,maxs,end,ENTITYNUM_NONE,MASK_PLAYERSOLID);
	if(trace.fraction < 1.0f && !trace.startsolid){
		VectorCopy(trace.endpos,origin);
	}
	return qtrue;
}

qboolean G_TrainingSpectatorSpawn(vec3_t origin,vec3_t angles){
	if(!trainingLive){return qfalse;}
	if(!G_RingDefined()){return qfalse;}
	G_RingVantage(origin,angles);
	return qtrue;
}

void G_TrainingEndFrame(gentity_t *ent){
	const rule_t *rule;
	int index,i;

	if(!trainingLive){return;}
	if(!ent->client){return;}
	// Before the dummy bail: a dummy has no lessons to be taught but it is a
	// fighter in the ring, and a sparring partner that could not be rung out
	// would make the mode unverifiable with one player.
	budokaiRingCheck(ent);
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

	G_Printf("rules loaded: %i from %s\n",G_RulesCount(),contentFile());
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
	if(G_RingDefined()){
		const ring_t *r = G_RingGet();
		G_Printf("ring: %.0f %.0f %.0f radius %.0f floor %.0f (file %s)\n",
			r->center[0],r->center[1],r->center[2],r->radius,r->floor,arenaFile());
	}else{
		G_Printf("ring: none on %s (file %s)\n",trainingMapName,arenaFile());
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

// -------------------------------------------------------------- journal
//
// The whole progression, sent to ONE client on request and never unasked. The
// authority is here - the tags, the ceiling and the loaded rule set all live on
// the server - so the client asks with `journal` and gets a batch back.
//
// Three commands, and a batch is bounded by the master count rather than by the
// lesson count:
//
//   trjournal <tierCeiling> <earnedTags> <lessonCount>
//   trjsec    <masterId> "<masterName>" "<lesson>|<lesson>|..."
//   trjend
//
// A <lesson> is one status char - d done, a available, l locked - followed
// immediately by the lesson's label. Master id 0 is the solo arc, the lessons no
// master is standing next to, and it is sent as a section like any other.
//
//   trjournal 2 4 10
//   trjsec 0 "" "dtraining.begun|dtrained.flight.hover|atrained.flight.endurance|ltrained.aura.sustain"
//   trjsec 1 "rhogan" "dtrained.rhogan.greeting|atrained.rhogan.flight"
//   trjsec 2 "oberak" "ltrained.oberak.greeting|ltrained.oberak.gravity"
//   trjend
//
// trjournal resets the client's page and trjend marks it complete, so a client
// drawing between the two knows it is looking at a partial batch. A master with
// more lessons than one command holds is sent as consecutive trjsec commands
// with the same id, which the client appends.
//
// MAX_RELIABLE_COMMANDS is 64 and overflow disconnects, so this is the one place
// in training mode that sends more than one command at a time and it is bounded
// twice: the request is rate limited per client, and the batch is capped at
// JOURNAL_MAX_COMMANDS whatever the content does.
#define JOURNAL_MAX_COMMANDS	24
// A held-down bind must not be able to spend the reliable command budget. The
// client asks once per open and retries only on silence, so this is generous.
#define JOURNAL_MIN_INTERVAL	500

static int earnedTagCount(const gclient_t *client){
	int	i,count;

	count = 0;
	for(i=0;i<G_TagCount();i++){
		if(G_TagTest(&client->pers.tags,i)){count++;}
	}
	return count;
}

// Sections are emitted in master order with the solo arc first, so the page
// reads the way the content ladder does: what you can do alone, then who taught
// you the rest.
static void sendJournal(gentity_t *ent){
	journalEntry_t	entries[MAX_JOURNAL_ENTRIES];
	char		packed[JOURNAL_PACK_SIZE];
	gclient_t	*client;
	const master_t	*master;
	const char	*name;
	int		clientNum,count,commands,section,masterId,cursor;

	client = ent->client;
	clientNum = ent - g_entities;
	count = G_JournalBuild(&client->pers.tags,client->pers.unlockedTier,entries,MAX_JOURNAL_ENTRIES);
	trap_SendServerCommand(clientNum,va("trjournal %i %i %i",
		tierCeiling(client),earnedTagCount(client),count));
	commands = 1;
	for(section=0;section<=G_MastersCount();section++){
		if(section == 0){
			masterId = JOURNAL_SOLO_MASTER;
			name = "";
		}
		else{
			master = G_MastersGet(section - 1);
			masterId = master->id;
			name = master->name;
		}
		cursor = 0;
		while(commands < JOURNAL_MAX_COMMANDS - 1){
			if(!G_JournalPack(entries,count,masterId,&cursor,packed,sizeof(packed))){break;}
			trap_SendServerCommand(clientNum,va("trjsec %i \"%s\" \"%s\"",masterId,name,packed));
			commands++;
		}
	}
	trap_SendServerCommand(clientNum,"trjend");
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
//   1. Declare the master once in GameData/rules/masters.def:  master 1 rhogan
//   2. Start the map with cheats on:  +set g_training 1 +set sv_cheats 1
//   3. Fly to where the master belongs and:  \masterplace rhogan 320
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

// ------------------------------------------------------------ arenaplace
//
// The ring editor, the same shape as masterplace and for the same reason: the
// maps are not in this repository, so the arena is authored by standing in it.
//
//   1. Start the map with cheats on:  +set g_training 1 +set sv_cheats 1
//   2. Stand in the middle of the ring, on its floor, and:  \arenaplace 1200
//      (the radius is optional and defaults to RING_DEFAULT_RADIUS)
//   3. \arenalist to check the radius against where you are standing
//   4. \arenasave writes rules/arena_<mapname>.def under fs_homepath
//   5. Copy that file into GameData/rules/ so zeq2build.sh stages it
//
// The floor comes from where the author is standing rather than from a trace:
// ps.origin sits at the middle of the bounding box, so the feet are one mins[2]
// below it, and an author who places the ring while flying is stating a floor
// in the air on purpose.

static void arenaPlace_f(gentity_t *ent){
	char arg[MAX_TOKEN_CHARS];
	vec3_t origin;
	float radius;

	trap_Argv(1,arg,sizeof(arg));
	radius = arg[0] ? atof(arg) : RING_DEFAULT_RADIUS;
	VectorCopy(ent->client->ps.origin,origin);
	if(!G_RingPlace(origin,radius,origin[2] - RING_PLACE_FLOOR_DROP)){
		trap_SendServerCommand(ent-g_entities,va("print \"arenaplace: %s\n\"",G_RingError()));
		return;
	}
	trap_SendServerCommand(ent-g_entities,va("print \"arenaplace: ring at %.0f %.0f %.0f radius %.0f floor %.0f - arenasave to keep it\n\"",
		origin[0],origin[1],origin[2],radius,origin[2] - RING_PLACE_FLOOR_DROP));
}

static void arenaSave_f(gentity_t *ent){
	if(!G_RingWrite(arenaFile(),trainingMapName)){
		trap_SendServerCommand(ent-g_entities,va("print \"arenasave: %s\n\"",G_RingError()));
		return;
	}
	trap_SendServerCommand(ent-g_entities,va("print \"arenasave: wrote %s\n\"",arenaFile()));
}

static void arenaList_f(gentity_t *ent){
	const ring_t *r;

	if(!G_RingDefined()){
		trap_SendServerCommand(ent-g_entities,va("print \"no ring on %s (%s)\n\"",trainingMapName,arenaFile()));
		return;
	}
	r = G_RingGet();
	trap_SendServerCommand(ent-g_entities,va("print \"ring at %.0f %.0f %.0f radius %.0f floor %.0f\n\"",
		r->center[0],r->center[1],r->center[2],r->radius,r->floor));
	trap_SendServerCommand(ent-g_entities,va("print \"  you are %.0f past the edge, %.0f above the floor\n\"",
		G_RingDistance(ent->client->ps.origin),G_RingHeight(ent->client->ps.origin)));
}

// Authoring commands are cheat-gated for the same reason setviewpos is: they
// move the world, and a public server is not an editing session.
qboolean G_TrainingClientCommand(gentity_t *ent,const char *cmd){
	// The journal is not authoring and is not cheat-gated: it reads back what
	// this client already earned. Answered only while the mode is live, so a
	// server with training off leaves the command unknown, as it is.
	if(!Q_stricmp(cmd,"journal")){
		if(!trainingLive){return qfalse;}
		if(level.time - ent->client->pers.journalTime < JOURNAL_MIN_INTERVAL &&
			ent->client->pers.journalTime){
			return qtrue;
		}
		ent->client->pers.journalTime = level.time;
		sendJournal(ent);
		return qtrue;
	}
	if(Q_stricmp(cmd,"masterplace") && Q_stricmp(cmd,"mastersave") && Q_stricmp(cmd,"masterlist") &&
		Q_stricmp(cmd,"arenaplace") && Q_stricmp(cmd,"arenasave") && Q_stricmp(cmd,"arenalist")){
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
	if(!Q_stricmp(cmd,"masterlist")){
		masterList_f(ent);
		return qtrue;
	}
	if(!Q_stricmp(cmd,"arenaplace")){
		arenaPlace_f(ent);
		return qtrue;
	}
	if(!Q_stricmp(cmd,"arenasave")){
		arenaSave_f(ent);
		return qtrue;
	}
	arenaList_f(ent);
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
