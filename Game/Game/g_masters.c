#include "g_local.h"

// Master vocabulary, per-map placement and the nearest-master search behind the
// masterNear fact.
//
// The file is written against the same failure mode as g_rules.c: a name that
// parses to nothing. An undeclared master in a placement file is an error with
// a file:line, not a silently skipped line, because a lesson keyed on a master
// nobody placed simply never fires and nothing says why.

static master_t	masters[MAX_MASTERS];
static int	masterCount;

// Index 0 is "none" so the table can be indexed by id directly. Kept beside the
// names rather than pointing into masters[] so a caller cannot reach the origins
// through the rule parser's value table.
static const char	*masterVocab[MAX_MASTERS + 1];

static char	masterError[MAX_MASTERS_ERROR];
static char	masterBuffer[MAX_MASTERS_FILE];

// ------------------------------------------------------------- diagnostics

static void QDECL mastersError_f(const char *file,int line,const char *fmt,...){
	va_list argptr;
	char text[MAX_MASTERS_ERROR];

	// First error wins, as in g_rules.c: the one that names the actual mistake
	// is the first, and later ones are its consequences.
	if(masterError[0]){return;}
	va_start(argptr,fmt);
	Q_vsnprintf(text,sizeof(text),fmt,argptr);
	va_end(argptr);
	Com_sprintf(masterError,sizeof(masterError),"%s:%i: %s",file,line,text);
	G_Printf("%s\n",masterError);
}

const char *G_MastersError(void){
	return masterError;
}

// ------------------------------------------------------------------ access

void G_MastersReset(void){
	memset(masters,0,sizeof(masters));
	memset(masterVocab,0,sizeof(masterVocab));
	masterCount = 0;
	masterError[0] = 0;
	masterVocab[0] = "none";
}

int G_MastersCount(void){
	return masterCount;
}

const master_t *G_MastersGet(int index){
	if(index < 0 || index >= masterCount){return NULL;}
	return &masters[index];
}

const master_t *G_MastersFind(const char *name){
	int i;
	for(i=0;i<masterCount;i++){
		if(!Q_stricmp(masters[i].name,name)){return &masters[i];}
	}
	return NULL;
}

const char *G_MastersName(int id){
	int i;
	if(id <= 0){return "none";}
	for(i=0;i<masterCount;i++){
		if(masters[i].id == id){return masters[i].name;}
	}
	return "";
}

const char *const *G_MastersVocabulary(int *count){
	int i,highest;

	masterVocab[0] = "none";
	highest = 0;
	for(i=0;i<masterCount;i++){
		masterVocab[masters[i].id] = masters[i].name;
		if(masters[i].id > highest){highest = masters[i].id;}
	}
	*count = highest + 1;
	return masterVocab;
}

// Nearest wins on overlap, which is the only tie-break that stays sensible when
// an author drops two masters in one room: the readout follows the one you
// walked up to.
int G_MastersNearest(const vec3_t origin){
	vec3_t delta;
	float distance,best;
	int i,id;

	id = 0;
	best = 0;
	for(i=0;i<masterCount;i++){
		if(!masters[i].placed){continue;}
		VectorSubtract(origin,masters[i].origin,delta);
		distance = VectorLength(delta);
		if(distance > masters[i].radius){continue;}
		if(id && distance >= best){continue;}
		id = masters[i].id;
		best = distance;
	}
	return id;
}

// ----------------------------------------------------------------- parsing

typedef struct {
	char		*data;
	const char	*file;
	int		line;
	int		tokenLine;
} mastersParse_t;

// COM_Parse skips whitespace and comments without reporting what it stepped
// over, so the line number is recovered by counting what the pointer passed.
static const char *mastersToken(mastersParse_t *p){
	const char *before,*c;
	const char *token;

	before = p->data;
	token = COM_Parse(&p->data);
	if(!p->data){p->data = (char *)before + strlen(before);}
	for(c = before;c < p->data;c++){
		if(*c == '\n'){p->line++;}
	}
	p->tokenLine = p->line;
	return token;
}

qboolean G_MastersParseDef(char *text,const char *file){
	mastersParse_t p;
	const char *token;
	int id,i;

	memset(&p,0,sizeof(p));
	p.data = text;
	p.file = file;
	p.line = 1;

	while(1){
		token = mastersToken(&p);
		if(!token[0]){break;}
		if(Q_stricmp(token,"master")){
			mastersError_f(p.file,p.tokenLine,"unknown declaration '%s' - did you mean 'master'?",token);
			return qfalse;
		}
		token = mastersToken(&p);
		if(!token[0] || !isdigit((int)token[0])){
			mastersError_f(p.file,p.tokenLine,"'master' needs an id, got '%s'",token);
			return qfalse;
		}
		id = atoi(token);
		// Ids are what travels in PERS_TRAINING_MASTER and what a rule compiles
		// to, so they are stated rather than implied by file order: reordering
		// the file must not renumber a master out from under saved content.
		if(id < 1 || id > MAX_MASTERS){
			mastersError_f(p.file,p.tokenLine,"master id %i out of range 1..%i",id,MAX_MASTERS);
			return qfalse;
		}
		token = mastersToken(&p);
		if(!token[0]){
			mastersError_f(p.file,p.tokenLine,"master %i has no name",id);
			return qfalse;
		}
		if((int)strlen(token) >= MAX_MASTER_NAME){
			mastersError_f(p.file,p.tokenLine,"master name '%s' longer than %i characters",token,MAX_MASTER_NAME-1);
			return qfalse;
		}
		for(i=0;i<masterCount;i++){
			if(masters[i].id == id){
				mastersError_f(p.file,p.tokenLine,"master id %i is already '%s'",id,masters[i].name);
				return qfalse;
			}
			if(!Q_stricmp(masters[i].name,token)){
				mastersError_f(p.file,p.tokenLine,"master '%s' declared twice",token);
				return qfalse;
			}
		}
		if(masterCount >= MAX_MASTERS){
			mastersError_f(p.file,p.tokenLine,"more than %i masters declared",MAX_MASTERS);
			return qfalse;
		}
		Q_strncpyz(masters[masterCount].name,token,MAX_MASTER_NAME);
		masters[masterCount].id = id;
		masters[masterCount].radius = MASTER_DEFAULT_RADIUS;
		masters[masterCount].placed = qfalse;
		masterCount++;
	}
	return qtrue;
}

static qboolean parseNumber(mastersParse_t *p,const char *what,float *out){
	const char *token;
	token = mastersToken(p);
	if(!token[0] || (!isdigit((int)token[0]) && token[0] != '-' && token[0] != '+' && token[0] != '.')){
		mastersError_f(p->file,p->tokenLine,"%s: expected a number, got '%s'",what,token);
		return qfalse;
	}
	*out = atof(token);
	return qtrue;
}

qboolean G_MastersParsePlacements(char *text,const char *file){
	mastersParse_t p;
	const char *token;
	master_t *master;
	vec3_t origin;
	float radius;
	int i;

	memset(&p,0,sizeof(p));
	p.data = text;
	p.file = file;
	p.line = 1;

	while(1){
		token = mastersToken(&p);
		if(!token[0]){break;}
		if(Q_stricmp(token,"place")){
			mastersError_f(p.file,p.tokenLine,"unknown declaration '%s' - did you mean 'place'?",token);
			return qfalse;
		}
		token = mastersToken(&p);
		if(!token[0]){
			mastersError_f(p.file,p.tokenLine,"'place' with no master name");
			return qfalse;
		}
		master = NULL;
		for(i=0;i<masterCount;i++){
			if(!Q_stricmp(masters[i].name,token)){
				master = &masters[i];
				break;
			}
		}
		if(!master){
			mastersError_f(p.file,p.tokenLine,"master '%s' not declared in masters.def",token);
			return qfalse;
		}
		for(i=0;i<3;i++){
			if(!parseNumber(&p,va("master '%s' origin",master->name),&origin[i])){return qfalse;}
		}
		if(!parseNumber(&p,va("master '%s' radius",master->name),&radius)){return qfalse;}
		if(radius <= 0){
			mastersError_f(p.file,p.tokenLine,"master '%s' has radius %.0f - a master nobody can reach",master->name,radius);
			return qfalse;
		}
		VectorCopy(origin,master->origin);
		master->radius = radius;
		master->placed = qtrue;
	}
	return qtrue;
}

// ----------------------------------------------------------------- loading

static int loadFile(const char *path){
	fileHandle_t f;
	int length;
	length = trap_FS_FOpenFile(path,&f,FS_READ);
	if(length < 0){return -1;}
	if(length >= MAX_MASTERS_FILE){
		trap_FS_FCloseFile(f);
		return -2;
	}
	trap_FS_Read(masterBuffer,length,f);
	masterBuffer[length] = 0;
	trap_FS_FCloseFile(f);
	return length;
}

qboolean G_MastersLoadDef(const char *path){
	int length;

	length = loadFile(path);
	if(length == -1){
		Com_sprintf(masterError,sizeof(masterError),"%s: master vocabulary not found",path);
		G_Printf("%s\n",masterError);
		return qfalse;
	}
	if(length == -2){
		Com_sprintf(masterError,sizeof(masterError),"%s: master vocabulary larger than %i bytes",path,MAX_MASTERS_FILE);
		G_Printf("%s\n",masterError);
		return qfalse;
	}
	return G_MastersParseDef(masterBuffer,path);
}

qboolean G_MastersLoadPlacements(const char *path){
	int length;

	length = loadFile(path);
	// A map with no placement file has no masters on it, which is a fact about
	// the map rather than a fault in the content.
	if(length == -1){return qtrue;}
	if(length == -2){
		Com_sprintf(masterError,sizeof(masterError),"%s: placement file larger than %i bytes",path,MAX_MASTERS_FILE);
		G_Printf("%s\n",masterError);
		return qfalse;
	}
	return G_MastersParsePlacements(masterBuffer,path);
}

// --------------------------------------------------------------- authoring

qboolean G_MastersPlace(const char *name,const vec3_t origin,float radius){
	int i;

	for(i=0;i<masterCount;i++){
		if(Q_stricmp(masters[i].name,name)){continue;}
		VectorCopy(origin,masters[i].origin);
		masters[i].radius = radius > 0 ? radius : MASTER_DEFAULT_RADIUS;
		masters[i].placed = qtrue;
		return qtrue;
	}
	Com_sprintf(masterError,sizeof(masterError),"master '%s' is not declared in masters.def",name);
	return qfalse;
}

qboolean G_MastersWrite(const char *path,const char *mapname){
	fileHandle_t f;
	char line[256];
	int i,written;

	trap_FS_FOpenFile(path,&f,FS_WRITE);
	if(!f){
		Com_sprintf(masterError,sizeof(masterError),"could not open %s for writing",path);
		G_Printf("%s\n",masterError);
		return qfalse;
	}
	Com_sprintf(line,sizeof(line),"// Master placement for %s, written by mastersave.\n"
		"// Names come from rules/masters.def; edit there to add one.\n\n",mapname);
	trap_FS_Write(line,(int)strlen(line),f);
	written = 0;
	for(i=0;i<masterCount;i++){
		if(!masters[i].placed){continue;}
		Com_sprintf(line,sizeof(line),"place  %s  %.0f %.0f %.0f  %.0f\n",masters[i].name,
			masters[i].origin[0],masters[i].origin[1],masters[i].origin[2],masters[i].radius);
		trap_FS_Write(line,(int)strlen(line),f);
		written++;
	}
	trap_FS_FCloseFile(f);
	G_Printf("mastersave: wrote %i placement(s) to %s\n",written,path);
	return qtrue;
}
