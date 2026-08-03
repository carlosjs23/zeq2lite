#include "g_local.h"

// Rule engine core: the tag vocabulary, the .rules parser, the matcher and the
// executor for inline test vectors.
//
// The failure mode the whole file is written against is the silent no-op - a
// typo'd fact or tag that makes a rule never match and is noticed a month
// later. So every name is declared somewhere and anything undeclared stops the
// load with a file:line and a suggestion, rather than parsing to nothing.

// ---------------------------------------------------------------- vocabulary

// A fact carrying a unit rejects bare numbers: `atLeast 45` is unreadable and
// `atLeast 45s` cannot be misread. altUnit exists so an author can state
// milliseconds where seconds are too coarse.
typedef struct {
	const char		*name;
	const char		*unit;
	int			unitScale;
	const char		*altUnit;
	int			altScale;
	const char *const	*values;
	int			numValues;
} factDef_t;

// Stock Quake III gravity is 800, and that is the 1g the movement code means.
#define GRAVITY_PER_G	800

// The masterNear vocabulary is the one fact table that is not fixed at compile
// time: it comes from rules/masters.def so that adding a master is content work
// rather than a C change. The built-in table below is the fallback for a tree
// with no masters.def and MUST agree with the shipped ids, since a rule
// compiles `masterNear is rhogan` to the index in this table.
static const char *const masterValues[] = {"none","rhogan","oberak"};
static const char *const roundValues[] = {"none","waiting","inProgress","over"};

// Indexed by factKey_t. Not const: G_RulesSetMasterVocabulary rewrites the
// masterNear entry's value table before content is parsed.
static factDef_t factDefs[fFactCount] = {
	{"powerCurrent",	"pl",	1,		NULL,	0,	NULL,		0},
	{"powerPercent",	"%",	1,		NULL,	0,	NULL,		0},
	{"fatigue",		"pl",	1,		NULL,	0,	NULL,		0},
	{"health",		"pl",	1,		NULL,	0,	NULL,		0},
	{"tierCurrent",		NULL,	0,		NULL,	0,	NULL,		0},
	{"tierTotal",		NULL,	0,		NULL,	0,	NULL,		0},
	{"gravity",		"g",	GRAVITY_PER_G,	NULL,	0,	NULL,		0},
	{"struggleEnergy",	NULL,	0,		NULL,	0,	NULL,		0},
	{"powerRaiseTime",	"s",	1000,		"ms",	1,	NULL,		0},
	{"airborneTime",	"s",	1000,		"ms",	1,	NULL,		0},
	{"auraTime",		"s",	1000,		"ms",	1,	NULL,		0},
	{"masterNear",		NULL,	0,		NULL,	0,	masterValues,	3},
	// Both are world units and both are signed: ringDistance is how far PAST the
	// ring edge the player is, so inside is negative, and ringHeight is measured
	// from the ring floor, so below it is negative. Plain numbers rather than a
	// unit, because there is no name for a Quake unit that an author would type.
	{"ringDistance",	NULL,	0,		NULL,	0,	NULL,		0},
	{"ringHeight",		NULL,	0,		NULL,	0,	NULL,		0}
};

// Indexed by worldFactKey_t.
static const factDef_t worldFactDefs[fWorldFactCount] = {
	{"roundState",	NULL,	0,	NULL,	0,	roundValues,	4},
	{"roundTime",	"s",	1000,	"ms",	1,	NULL,		0},
	{"eventTimer",	"s",	1000,	"ms",	1,	NULL,		0},
	{"scoreRed",	NULL,	0,	NULL,	0,	NULL,		0},
	{"scoreBlue",	NULL,	0,	NULL,	0,	NULL,		0}
};

static const char *const actionNames[acActionCount] = {
	"grant","remove","say","objective","setGravity","unlock"
};

// ------------------------------------------------------------------- storage

int		g_worldFacts[fWorldFactCount];
tagSet_t	g_worldTags;

static char	tagNames[MAX_TAG_BITS][MAX_TAG_NAME];
static int	tagCount;

typedef struct {
	char	prefix[MAX_TAG_NAME];
	int	base;		// first bit of the reserved run
	int	used;		// bits handed out inside it
} tagGroup_t;

static tagGroup_t	tagGroups[MAX_TAG_GROUPS];
static int		tagGroupCount;

static rule_t		*rules;
static int		ruleCount;
static ruleTest_t	*ruleTests;
static int		ruleTestCount;

static char		rulesError[MAX_RULE_ERROR];
static char		fileBuffer[MAX_RULES_FILE];
static char		poolText[MAX_ACTION_TEXT];

// ---------------------------------------------------------------- tag sets

void G_TagSet(tagSet_t *set,int bit){
	if(bit < 0 || bit >= MAX_TAG_BITS){return;}
	set->bits[bit >> 5] |= 1u << (bit & 31);
}

void G_TagClear(tagSet_t *set,int bit){
	if(bit < 0 || bit >= MAX_TAG_BITS){return;}
	set->bits[bit >> 5] &= ~(1u << (bit & 31));
}

qboolean G_TagTest(const tagSet_t *set,int bit){
	if(bit < 0 || bit >= MAX_TAG_BITS){return qfalse;}
	return (set->bits[bit >> 5] & (1u << (bit & 31))) ? qtrue : qfalse;
}

qboolean G_TagsHaveAll(const tagSet_t *set,const tagSet_t *need){
	int i;
	for(i=0;i<MAX_TAG_WORDS;i++){
		if((need->bits[i] & ~set->bits[i])){return qfalse;}
	}
	return qtrue;
}

qboolean G_TagsHaveAny(const tagSet_t *set,const tagSet_t *any){
	int i;
	for(i=0;i<MAX_TAG_WORDS;i++){
		if(set->bits[i] & any->bits[i]){return qtrue;}
	}
	return qfalse;
}

qboolean G_TagsEmpty(const tagSet_t *set){
	int i;
	for(i=0;i<MAX_TAG_WORDS;i++){
		if(set->bits[i]){return qfalse;}
	}
	return qtrue;
}

int G_TagFind(const char *name){
	int i;
	for(i=0;i<tagCount;i++){
		if(!Q_stricmp(tagNames[i],name)){return i;}
	}
	return -1;
}

const char *G_TagName(int bit){
	if(bit < 0 || bit >= tagCount){return "";}
	return tagNames[bit];
}

int G_TagCount(void){
	return tagCount;
}

// ------------------------------------------------------------- diagnostics

// Classic edit distance over a two-row window, so the whole thing fits the QVM
// stack. Names longer than the window degrade to "no suggestion", never to a
// wrong one.
#define SUGGEST_MAX	64

static int levenshtein(const char *a,const char *b){
	int prev[SUGGEST_MAX+1],cur[SUGGEST_MAX+1];
	int la,lb,i,j,cost,best;
	la = (int)strlen(a);
	lb = (int)strlen(b);
	if(la > SUGGEST_MAX || lb > SUGGEST_MAX){return SUGGEST_MAX+1;}
	for(j=0;j<=lb;j++){prev[j] = j;}
	for(i=1;i<=la;i++){
		cur[0] = i;
		for(j=1;j<=lb;j++){
			cost = (tolower(a[i-1]) == tolower(b[j-1])) ? 0 : 1;
			best = prev[j] + 1;
			if(cur[j-1] + 1 < best){best = cur[j-1] + 1;}
			if(prev[j-1] + cost < best){best = prev[j-1] + cost;}
			cur[j] = best;
		}
		for(j=0;j<=lb;j++){prev[j] = cur[j];}
	}
	return prev[lb];
}

// Closest candidate, or NULL when nothing is close enough to be worth printing.
// A suggestion further away than three edits is noise and reads as a taunt.
static const char *suggest(const char *word,const char *const *candidates,int count){
	int i,d,bestDistance;
	const char *best;
	best = NULL;
	bestDistance = 4;
	for(i=0;i<count;i++){
		if(!candidates[i]){continue;}
		d = levenshtein(word,candidates[i]);
		if(d < bestDistance){
			bestDistance = d;
			best = candidates[i];
		}
	}
	return best;
}

static const char *suggestFact(const char *word,qboolean world){
	const char *names[fFactCount + fWorldFactCount];
	int i,count;
	count = world ? fWorldFactCount : fFactCount;
	for(i=0;i<count;i++){
		names[i] = world ? worldFactDefs[i].name : factDefs[i].name;
	}
	return suggest(word,names,count);
}

static const char *suggestTag(const char *word){
	const char *names[MAX_TAG_BITS];
	int i;
	for(i=0;i<tagCount;i++){names[i] = tagNames[i];}
	return suggest(word,names,tagCount);
}

// First error wins: later ones are usually consequences of it, and a parser
// that keeps going past an undeclared name reports fiction.
static void QDECL rulesError_f(const char *file,int line,const char *fmt,...){
	va_list argptr;
	char text[MAX_RULE_ERROR];
	if(rulesError[0]){return;}
	va_start(argptr,fmt);
	Q_vsnprintf(text,sizeof(text),fmt,argptr);
	va_end(argptr);
	Com_sprintf(rulesError,sizeof(rulesError),"%s:%i: %s",file,line,text);
	G_Printf("%s\n",rulesError);
}

const char *G_RulesError(void){
	return rulesError;
}

// ----------------------------------------------------------------- tokenizer

// COM_Parse tracks lines by counting newlines as it walks off the end of a
// token, so the line it reports for a token at end of line is already the next
// one. Errors are the entire point of this language, so the whitespace and
// comment skip is done here where the line of the token's *first* character is
// still knowable.
typedef struct {
	char		*data;
	const char	*file;
	int		line;
	int		tokenLine;
} rulesParse_t;

// Advance to the next token's first character. With sameLine set the newline is
// left unconsumed, so a clause reading an argument list stops at end of line and
// the enclosing loop still sees the next keyword.
static qboolean rulesSkipWhite(rulesParse_t *p,qboolean sameLine){
	char *d,*s;
	int line,crossed;
	d = p->data;
	line = p->line;
	while(*d){
		if(*d == '\n'){
			if(sameLine){break;}
			line++;
			d++;
			continue;
		}
		if(*d > 0 && *d <= ' '){d++; continue;}
		if(d[0] == '/' && d[1] == '/'){
			while(*d && *d != '\n'){d++;}
			continue;
		}
		if(d[0] == '/' && d[1] == '*'){
			crossed = 0;
			s = d + 2;
			while(*s && !(s[0] == '*' && s[1] == '/')){
				if(*s == '\n'){crossed++;}
				s++;
			}
			if(*s){s += 2;}
			if(sameLine && crossed){break;}
			line += crossed;
			d = s;
			continue;
		}
		break;
	}
	p->data = d;
	p->line = line;
	if(!*d || (sameLine && *d == '\n')){return qfalse;}
	return qtrue;
}

static char rulesEmptyToken[1];

// Next token, or "" at end of input (or end of line, with sameLine set).
static const char *rulesToken(rulesParse_t *p,qboolean sameLine){
	char *before,*c,*token;
	if(!rulesSkipWhite(p,sameLine)){
		rulesEmptyToken[0] = 0;
		return rulesEmptyToken;
	}
	p->tokenLine = p->line;
	before = p->data;
	token = COM_Parse(&p->data);
	if(!p->data){p->data = before + strlen(before);}
	// Only a quoted string can span lines; count what it swallowed.
	for(c = before;c < p->data;c++){
		if(*c == '\n'){p->line++;}
	}
	return token;
}

// ------------------------------------------------------------------- loading

static int loadFile(const char *path){
	fileHandle_t f;
	int length;
	length = trap_FS_FOpenFile(path,&f,FS_READ);
	if(length < 0){return -1;}
	if(length >= MAX_RULES_FILE){
		trap_FS_FCloseFile(f);
		return -2;
	}
	trap_FS_Read(fileBuffer,length,f);
	fileBuffer[length] = 0;
	trap_FS_FCloseFile(f);
	return length;
}

// ------------------------------------------------------------ tag vocabulary

// Everything before the last dot. A tag with no dot belongs to the root group.
static void tagPrefixOf(const char *name,char *out,int outSize){
	const char *dot;
	int length;
	dot = strrchr(name,'.');
	if(!dot){
		out[0] = 0;
		return;
	}
	length = (int)(dot - name) + 1;
	if(length > outSize){length = outSize;}
	Q_strncpyz(out,name,length);
}

static tagGroup_t *tagGroupFor(const char *prefix){
	int i;
	for(i=0;i<tagGroupCount;i++){
		if(!Q_stricmp(tagGroups[i].prefix,prefix)){return &tagGroups[i];}
	}
	return NULL;
}

qboolean G_TagPrefixMask(const char *prefix,tagSet_t *out){
	tagGroup_t *group;
	int i;
	memset(out,0,sizeof(*out));
	group = tagGroupFor(prefix);
	if(!group){return qfalse;}
	for(i=0;i<group->used;i++){
		G_TagSet(out,group->base + i);
	}
	return qtrue;
}

static qboolean parseTagsDef(const char *path){
	rulesParse_t p;
	const char *token;
	char prefix[MAX_TAG_NAME];
	tagGroup_t *group;
	int length;

	length = loadFile(path);
	if(length == -1){
		Com_sprintf(rulesError,sizeof(rulesError),"%s: tag vocabulary not found",path);
		G_Printf("%s\n",rulesError);
		return qfalse;
	}
	if(length == -2){
		Com_sprintf(rulesError,sizeof(rulesError),"%s: tag vocabulary larger than %i bytes",path,MAX_RULES_FILE);
		G_Printf("%s\n",rulesError);
		return qfalse;
	}

	memset(&p,0,sizeof(p));
	p.data = fileBuffer;
	p.file = path;
	p.line = 1;

	while(1){
		token = rulesToken(&p,qfalse);
		if(!token[0]){break;}
		if(Q_stricmp(token,"tag")){
			rulesError_f(p.file,p.tokenLine,"unknown declaration '%s' - did you mean 'tag'?",token);
			return qfalse;
		}
		token = rulesToken(&p,qtrue);
		if(!token[0]){
			rulesError_f(p.file,p.tokenLine,"'tag' with no name");
			return qfalse;
		}
		if((int)strlen(token) >= MAX_TAG_NAME){
			rulesError_f(p.file,p.tokenLine,"tag '%s' longer than %i characters",token,MAX_TAG_NAME-1);
			return qfalse;
		}
		if(G_TagFind(token) >= 0){
			rulesError_f(p.file,p.tokenLine,"tag '%s' declared twice",token);
			return qfalse;
		}
		tagPrefixOf(token,prefix,sizeof(prefix));
		group = tagGroupFor(prefix);
		if(!group){
			if(tagGroupCount >= MAX_TAG_GROUPS){
				rulesError_f(p.file,p.tokenLine,"tag budget exhausted (%i of %i used)",tagGroupCount * TAG_GROUP_BITS,MAX_TAG_BITS);
				return qfalse;
			}
			group = &tagGroups[tagGroupCount++];
			Q_strncpyz(group->prefix,prefix,sizeof(group->prefix));
			group->base = (tagGroupCount - 1) * TAG_GROUP_BITS;
			group->used = 0;
		}
		if(group->used >= TAG_GROUP_BITS){
			rulesError_f(p.file,p.tokenLine,"tag prefix group '%s' exhausted (%i of %i used)",prefix,group->used,TAG_GROUP_BITS);
			return qfalse;
		}
		Q_strncpyz(tagNames[group->base + group->used],token,MAX_TAG_NAME);
		group->used++;
		if(group->base + group->used > tagCount){
			tagCount = group->base + group->used;
		}
	}
	return qtrue;
}

// -------------------------------------------------------------- value parsing

// Split "45s" into 45 and "s". A leading sign and a decimal point belong to the
// number; everything from the first other character is the unit.
static qboolean splitValue(const char *token,float *number,char *unit,int unitSize){
	int i;
	char digits[32];
	i = 0;
	if(token[i] == '-' || token[i] == '+'){i++;}
	if(!isdigit((int)token[i])){return qfalse;}
	while(isdigit((int)token[i]) || token[i] == '.'){i++;}
	if(i >= (int)sizeof(digits)){return qfalse;}
	Q_strncpyz(digits,token,i+1);
	*number = atof(digits);
	Q_strncpyz(unit,token+i,unitSize);
	return qtrue;
}

// A fact's declared vocabulary is the whole check: an enum fact takes only its
// named values, a united fact rejects bare numbers, a plain fact rejects units.
static qboolean parseFactValue(rulesParse_t *p,const factDef_t *def,const char *token,int *out){
	float number;
	char unit[16];
	const char *hint;
	int i;

	if(def->values){
		for(i=0;i<def->numValues;i++){
			if(!Q_stricmp(token,def->values[i])){
				*out = i;
				return qtrue;
			}
		}
		hint = suggest(token,def->values,def->numValues);
		if(hint){
			rulesError_f(p->file,p->tokenLine,"unknown value '%s' for fact '%s' - did you mean '%s'?",token,def->name,hint);
		}else{
			rulesError_f(p->file,p->tokenLine,"unknown value '%s' for fact '%s'",token,def->name);
		}
		return qfalse;
	}
	if(!splitValue(token,&number,unit,sizeof(unit))){
		rulesError_f(p->file,p->tokenLine,"'%s' is not a number",token);
		return qfalse;
	}
	if(!def->unit){
		if(unit[0]){
			rulesError_f(p->file,p->tokenLine,"fact '%s' takes a plain number, not '%s'",def->name,token);
			return qfalse;
		}
		*out = (int)number;
		return qtrue;
	}
	if(!unit[0]){
		rulesError_f(p->file,p->tokenLine,"bare number '%s' for fact '%s' - did you mean '%s%s'?",token,def->name,token,def->unit);
		return qfalse;
	}
	if(!Q_stricmp(unit,def->unit)){
		*out = (int)(number * def->unitScale);
		return qtrue;
	}
	if(def->altUnit && !Q_stricmp(unit,def->altUnit)){
		*out = (int)(number * def->altScale);
		return qtrue;
	}
	rulesError_f(p->file,p->tokenLine,"unknown unit '%s' for fact '%s' - expected '%s'",unit,def->name,def->unit);
	return qfalse;
}

// For the handful of action arguments that are not fact values - a tier index
// carries no unit and never will.
static qboolean parsePlainInt(rulesParse_t *p,const char *token,const char *what,int *out){
	float number;
	char unit[16];
	if(!splitValue(token,&number,unit,sizeof(unit)) || unit[0]){
		rulesError_f(p->file,p->tokenLine,"'%s' takes a plain number, not '%s'",what,token);
		return qfalse;
	}
	*out = (int)number;
	return qtrue;
}

// Reads an optional `world` qualifier and the fact name after it.
static qboolean parseFactRef(rulesParse_t *p,const char *token,int *key,const factDef_t **def){
	qboolean world;
	const char *hint;
	int i,count;

	world = qfalse;
	if(!Q_stricmp(token,"world")){
		world = qtrue;
		token = rulesToken(p,qtrue);
		if(!token[0]){
			rulesError_f(p->file,p->tokenLine,"'world' with no fact name");
			return qfalse;
		}
	}
	count = world ? fWorldFactCount : fFactCount;
	for(i=0;i<count;i++){
		const factDef_t *candidate = world ? &worldFactDefs[i] : &factDefs[i];
		if(!Q_stricmp(token,candidate->name)){
			*key = world ? (i | RULE_WORLD_KEY) : i;
			*def = candidate;
			return qtrue;
		}
	}
	hint = suggestFact(token,world);
	if(hint){
		rulesError_f(p->file,p->tokenLine,"unknown fact '%s' - did you mean '%s'?",token,hint);
	}else{
		rulesError_f(p->file,p->tokenLine,"unknown fact '%s'",token);
	}
	return qfalse;
}

int G_RulesFactKey(const char *name){
	int i;
	for(i=0;i<fFactCount;i++){
		if(!Q_stricmp(factDefs[i].name,name)){return i;}
	}
	for(i=0;i<fWorldFactCount;i++){
		if(!Q_stricmp(worldFactDefs[i].name,name)){return i | RULE_WORLD_KEY;}
	}
	return -1;
}

const char *G_RulesFactName(int key){
	if(key & RULE_WORLD_KEY){
		key &= ~RULE_WORLD_KEY;
		if(key < 0 || key >= fWorldFactCount){return "";}
		return worldFactDefs[key].name;
	}
	if(key < 0 || key >= fFactCount){return "";}
	return factDefs[key].name;
}

const char *G_RulesFactUnit(int key){
	if(key & RULE_WORLD_KEY){
		key &= ~RULE_WORLD_KEY;
		if(key < 0 || key >= fWorldFactCount){return NULL;}
		return worldFactDefs[key].unit;
	}
	if(key < 0 || key >= fFactCount){return NULL;}
	return factDefs[key].unit;
}

const char *G_RulesFactValue(int key,int index){
	const factDef_t *def;
	if(key & RULE_WORLD_KEY){
		key &= ~RULE_WORLD_KEY;
		if(key < 0 || key >= fWorldFactCount){return NULL;}
		def = &worldFactDefs[key];
	}else{
		if(key < 0 || key >= fFactCount){return NULL;}
		def = &factDefs[key];
	}
	if(!def->values || index < 0 || index >= def->numValues){return NULL;}
	return def->values[index];
}

// ------------------------------------------------------------- tag clauses

// A `.*` suffix names a declared prefix group. Wildcards over several groups
// are rejected rather than silently matching a non-contiguous set - the whole
// reason bits are allocated by prefix is that a prefix is one mask.
static qboolean parseTagInto(rulesParse_t *p,const char *token,tagSet_t *set){
	char prefix[MAX_TAG_NAME];
	tagSet_t mask;
	int length,bit,word;
	const char *hint;

	length = (int)strlen(token);
	if(length >= 2 && token[length-1] == '*' && token[length-2] == '.'){
		// Drop the ".*" - a group's prefix is stored without its trailing dot.
		Q_strncpyz(prefix,token,length-1 > (int)sizeof(prefix) ? (int)sizeof(prefix) : length-1);
		if(!G_TagPrefixMask(prefix,&mask)){
			rulesError_f(p->file,p->tokenLine,"tag prefix '%s' not declared in tags.def",prefix);
			return qfalse;
		}
		for(word=0;word<MAX_TAG_WORDS;word++){
			set->bits[word] |= mask.bits[word];
		}
		return qtrue;
	}
	bit = G_TagFind(token);
	if(bit < 0){
		hint = suggestTag(token);
		if(hint){
			rulesError_f(p->file,p->tokenLine,"tag '%s' not declared in tags.def - did you mean '%s'?",token,hint);
		}else{
			rulesError_f(p->file,p->tokenLine,"tag '%s' not declared in tags.def",token);
		}
		return qfalse;
	}
	G_TagSet(set,bit);
	return qtrue;
}

static qboolean parseTagList(rulesParse_t *p,tagSet_t *set,const char *clause){
	const char *token;
	int count;
	count = 0;
	while(1){
		token = rulesToken(p,qtrue);
		if(!token[0]){break;}
		if(!parseTagInto(p,token,set)){return qfalse;}
		count++;
	}
	if(!count){
		rulesError_f(p->file,p->tokenLine,"'%s' with no tag",clause);
		return qfalse;
	}
	return qtrue;
}

// ----------------------------------------------------------------- criteria

static qboolean parseCriterion(rulesParse_t *p,rule_t *rule){
	const char *token;
	char op[32];
	const factDef_t *def;
	criterion_t *criterion;
	int key,value,second,i;

	if(rule->numCriteria >= MAX_RULE_CRITERIA){
		rulesError_f(p->file,p->tokenLine,"rule '%s' has more than %i criteria",rule->name,MAX_RULE_CRITERIA);
		return qfalse;
	}
	token = rulesToken(p,qtrue);
	if(!token[0]){
		rulesError_f(p->file,p->tokenLine,"'when' with no fact");
		return qfalse;
	}
	if(!parseFactRef(p,token,&key,&def)){return qfalse;}
	for(i=0;i<rule->numCriteria;i++){
		if(rule->criteria[i].key == key){
			rulesError_f(p->file,p->tokenLine,"fact '%s' constrained twice in rule '%s'",def->name,rule->name);
			return qfalse;
		}
	}
	token = rulesToken(p,qtrue);
	if(!token[0]){
		rulesError_f(p->file,p->tokenLine,"fact '%s' with no operator",def->name);
		return qfalse;
	}
	Q_strncpyz(op,token,sizeof(op));
	token = rulesToken(p,qtrue);
	if(!token[0]){
		rulesError_f(p->file,p->tokenLine,"operator '%s' with no value",op);
		return qfalse;
	}
	if(!parseFactValue(p,def,token,&value)){return qfalse;}

	criterion = &rule->criteria[rule->numCriteria];
	criterion->key = key;
	if(!Q_stricmp(op,"is")){
		criterion->min = value;
		criterion->max = value;
	}else if(!Q_stricmp(op,"atLeast")){
		criterion->min = value;
		criterion->max = MAX_QINT;
	}else if(!Q_stricmp(op,"atMost")){
		criterion->min = MIN_QINT;
		criterion->max = value;
	}else if(!Q_stricmp(op,"above")){
		criterion->min = value + 1;
		criterion->max = MAX_QINT;
	}else if(!Q_stricmp(op,"below")){
		criterion->min = MIN_QINT;
		criterion->max = value - 1;
	}else if(!Q_stricmp(op,"between")){
		token = rulesToken(p,qtrue);
		if(Q_stricmp(token,"and")){
			rulesError_f(p->file,p->tokenLine,"'between' needs 'and', got '%s'",token);
			return qfalse;
		}
		token = rulesToken(p,qtrue);
		if(!token[0]){
			rulesError_f(p->file,p->tokenLine,"'between %s and' with no upper bound",op);
			return qfalse;
		}
		if(!parseFactValue(p,def,token,&second)){return qfalse;}
		criterion->min = value;
		criterion->max = second;
	}else{
		static const char *const operators[] = {"is","atLeast","atMost","above","below","between"};
		const char *hint = suggest(op,operators,6);
		if(hint){
			rulesError_f(p->file,p->tokenLine,"unknown operator '%s' - did you mean '%s'?",op,hint);
		}else{
			rulesError_f(p->file,p->tokenLine,"unknown operator '%s'",op);
		}
		return qfalse;
	}
	if(criterion->min > criterion->max){
		rulesError_f(p->file,p->tokenLine,"criterion on '%s' in rule '%s' can never match",def->name,rule->name);
		return qfalse;
	}
	rule->numCriteria++;
	return qtrue;
}

// ------------------------------------------------------------------ actions

// Action text is pooled at load like everything else here; nothing is freed
// because a rule database is fully known once G_InitGame has read it.
static char *poolString(const char *text){
	int length;
	char *out;
	length = (int)strlen(text) + 1;
	if(length > MAX_ACTION_TEXT){length = MAX_ACTION_TEXT;}
	out = G_Alloc(length);
	Q_strncpyz(out,text,length);
	return out;
}

static qboolean parseAction(rulesParse_t *p,rule_t *rule,const char *keyword){
	action_t *action;
	const char *token;
	const factDef_t *def;
	const char *hint;
	int key,value,bit;

	if(rule->numActions >= MAX_RULE_ACTIONS){
		rulesError_f(p->file,p->tokenLine,"rule '%s' has more than %i actions",rule->name,MAX_RULE_ACTIONS);
		return qfalse;
	}
	action = &rule->actions[rule->numActions];
	memset(action,0,sizeof(*action));

	if(!Q_stricmp(keyword,"grant") || !Q_stricmp(keyword,"remove")){
		action->type = Q_stricmp(keyword,"grant") ? acRemove : acGrant;
		token = rulesToken(p,qtrue);
		if(!token[0]){
			rulesError_f(p->file,p->tokenLine,"'%s' with no tag",keyword);
			return qfalse;
		}
		bit = G_TagFind(token);
		if(bit < 0){
			hint = suggestTag(token);
			if(hint){
				rulesError_f(p->file,p->tokenLine,"tag '%s' not declared in tags.def - did you mean '%s'?",token,hint);
			}else{
				rulesError_f(p->file,p->tokenLine,"tag '%s' not declared in tags.def",token);
			}
			return qfalse;
		}
		action->tag = bit;
	}else if(!Q_stricmp(keyword,"say")){
		action->type = acSay;
		token = rulesToken(p,qtrue);
		if(!token[0]){
			rulesError_f(p->file,p->tokenLine,"'say' with no text");
			return qfalse;
		}
		action->text = poolString(token);
	}else if(!Q_stricmp(keyword,"objective")){
		action->type = acObjective;
		token = rulesToken(p,qtrue);
		if(!token[0]){
			rulesError_f(p->file,p->tokenLine,"'objective' with no text");
			return qfalse;
		}
		Q_strncpyz(poolText,token,sizeof(poolText));
		token = rulesToken(p,qtrue);
		if(Q_stricmp(token,"track")){
			rulesError_f(p->file,p->tokenLine,"'objective' needs 'track', got '%s'",token);
			return qfalse;
		}
		token = rulesToken(p,qtrue);
		if(!token[0]){
			rulesError_f(p->file,p->tokenLine,"'track' with no fact");
			return qfalse;
		}
		if(!parseFactRef(p,token,&key,&def)){return qfalse;}
		token = rulesToken(p,qtrue);
		if(Q_stricmp(token,"goal")){
			rulesError_f(p->file,p->tokenLine,"'objective' needs 'goal', got '%s'",token);
			return qfalse;
		}
		token = rulesToken(p,qtrue);
		if(!token[0]){
			rulesError_f(p->file,p->tokenLine,"'goal' with no value");
			return qfalse;
		}
		if(!parseFactValue(p,def,token,&value)){return qfalse;}
		action->text = poolString(poolText);
		action->track = key;
		action->value = value;
	}else if(!Q_stricmp(keyword,"setGravity")){
		action->type = acSetGravity;
		token = rulesToken(p,qtrue);
		if(!token[0]){
			rulesError_f(p->file,p->tokenLine,"'setGravity' with no value");
			return qfalse;
		}
		if(!parseFactValue(p,&factDefs[fGravity],token,&value)){return qfalse;}
		action->value = value;
	}else if(!Q_stricmp(keyword,"unlock")){
		action->type = acUnlockTier;
		token = rulesToken(p,qtrue);
		if(Q_stricmp(token,"tier")){
			rulesError_f(p->file,p->tokenLine,"'unlock' needs 'tier', got '%s'",token);
			return qfalse;
		}
		token = rulesToken(p,qtrue);
		if(!token[0]){
			rulesError_f(p->file,p->tokenLine,"'unlock tier' with no number");
			return qfalse;
		}
		if(!parsePlainInt(p,token,"unlock tier",&value)){return qfalse;}
		action->value = value;
	}else{
		hint = suggest(keyword,actionNames,acActionCount);
		if(hint){
			rulesError_f(p->file,p->tokenLine,"unknown action '%s' - did you mean '%s'?",keyword,hint);
		}else{
			rulesError_f(p->file,p->tokenLine,"unknown action '%s'",keyword);
		}
		return qfalse;
	}
	rule->numActions++;
	return qtrue;
}

// -------------------------------------------------------------------- rules

static qboolean parseRule(rulesParse_t *p){
	rule_t *rule;
	const char *token;

	if(ruleCount >= MAX_RULES){
		rulesError_f(p->file,p->tokenLine,"more than %i rules",MAX_RULES);
		return qfalse;
	}
	rule = &rules[ruleCount];
	memset(rule,0,sizeof(*rule));
	rule->line = p->tokenLine;

	token = rulesToken(p,qtrue);
	if(!token[0]){
		rulesError_f(p->file,p->tokenLine,"'rule' with no name");
		return qfalse;
	}
	if((int)strlen(token) >= MAX_RULE_NAME){
		rulesError_f(p->file,p->tokenLine,"rule name '%s' longer than %i characters",token,MAX_RULE_NAME-1);
		return qfalse;
	}
	if(G_RulesFind(token)){
		rulesError_f(p->file,p->tokenLine,"rule '%s' declared twice",token);
		return qfalse;
	}
	Q_strncpyz(rule->name,token,sizeof(rule->name));

	token = rulesToken(p,qfalse);
	if(Q_stricmp(token,"{")){
		rulesError_f(p->file,p->tokenLine,"rule '%s' needs '{', got '%s'",rule->name,token);
		return qfalse;
	}
	while(1){
		token = rulesToken(p,qfalse);
		if(!token[0]){
			rulesError_f(p->file,p->tokenLine,"rule '%s' is missing its '}'",rule->name);
			return qfalse;
		}
		if(!Q_stricmp(token,"}")){break;}
		if(!Q_stricmp(token,"when")){
			if(!parseCriterion(p,rule)){return qfalse;}
		}else if(!Q_stricmp(token,"requires")){
			if(!parseTagList(p,&rule->requireTags,"requires")){return qfalse;}
		}else if(!Q_stricmp(token,"forbids")){
			if(!parseTagList(p,&rule->forbidTags,"forbids")){return qfalse;}
		}else if(!parseAction(p,rule,token)){
			return qfalse;
		}
	}
	ruleCount++;
	return qtrue;
}

// --------------------------------------------------------------- test blocks

static qboolean parseTest(rulesParse_t *p){
	ruleTest_t *test;
	const char *token;
	const factDef_t *def;
	int key,value;

	if(ruleTestCount >= MAX_RULE_TESTS){
		rulesError_f(p->file,p->tokenLine,"more than %i tests",MAX_RULE_TESTS);
		return qfalse;
	}
	test = &ruleTests[ruleTestCount];
	memset(test,0,sizeof(*test));
	test->line = p->tokenLine;

	token = rulesToken(p,qtrue);
	if(!token[0]){
		rulesError_f(p->file,p->tokenLine,"'test' with no name");
		return qfalse;
	}
	Q_strncpyz(test->name,token,sizeof(test->name));
	token = rulesToken(p,qfalse);
	if(Q_stricmp(token,"{")){
		rulesError_f(p->file,p->tokenLine,"test '%s' needs '{', got '%s'",test->name,token);
		return qfalse;
	}
	while(1){
		token = rulesToken(p,qfalse);
		if(!token[0]){
			rulesError_f(p->file,p->tokenLine,"test '%s' is missing its '}'",test->name);
			return qfalse;
		}
		if(!Q_stricmp(token,"}")){break;}
		if(!Q_stricmp(token,"given")){
			token = rulesToken(p,qtrue);
			if(!token[0]){
				rulesError_f(p->file,p->tokenLine,"'given' with no fact");
				return qfalse;
			}
			if(!Q_stricmp(token,"tags")){
				if(!parseTagList(p,&test->tags,"given tags")){return qfalse;}
				continue;
			}
			if(!Q_stricmp(token,"worldTags")){
				if(!parseTagList(p,&test->worldTags,"given worldTags")){return qfalse;}
				continue;
			}
			if(!parseFactRef(p,token,&key,&def)){return qfalse;}
			token = rulesToken(p,qtrue);
			if(!token[0]){
				rulesError_f(p->file,p->tokenLine,"'given %s' with no value",def->name);
				return qfalse;
			}
			if(!parseFactValue(p,def,token,&value)){return qfalse;}
			if(key & RULE_WORLD_KEY){
				test->worldFacts[key & ~RULE_WORLD_KEY] = value;
			}else{
				test->facts[key] = value;
			}
		}else if(!Q_stricmp(token,"expect")){
			token = rulesToken(p,qtrue);
			if(!token[0]){
				rulesError_f(p->file,p->tokenLine,"'expect' with no rule name");
				return qfalse;
			}
			// `expect none` is the vector for "nothing should fire here", which
			// is the case content authors get wrong most often.
			if(!Q_stricmp(token,"none")){
				test->expect[0] = 0;
			}else{
				Q_strncpyz(test->expect,token,sizeof(test->expect));
			}
		}else{
			static const char *const clauses[] = {"given","expect"};
			const char *hint = suggest(token,clauses,2);
			if(hint){
				rulesError_f(p->file,p->tokenLine,"unknown test clause '%s' - did you mean '%s'?",token,hint);
			}else{
				rulesError_f(p->file,p->tokenLine,"unknown test clause '%s'",token);
			}
			return qfalse;
		}
	}
	ruleTestCount++;
	return qtrue;
}

// ----------------------------------------------------------------- validation

static qboolean rangesOverlap(const criterion_t *a,const criterion_t *b){
	if(a->max < b->min){return qfalse;}
	if(b->max < a->min){return qfalse;}
	return qtrue;
}

// A rule requiring a tag it also forbids can never match, so it cannot tie with
// anything. This is not the self-terminating shape - that one grants what it
// forbids, and matches exactly once.
static qboolean ruleSatisfiable(const rule_t *rule){
	return G_TagsHaveAny(&rule->requireTags,&rule->forbidTags) ? qfalse : qtrue;
}

// Two rules tie when they have the same criteria count and no clause makes them
// mutually exclusive - then some state matches both and the winner would be a
// coin flip. Rejecting at load is the whole reason the language exists.
static qboolean rulesCanTie(const rule_t *a,const rule_t *b){
	int i,j;
	if(a->numCriteria != b->numCriteria){return qfalse;}
	if(!ruleSatisfiable(a) || !ruleSatisfiable(b)){return qfalse;}
	if(G_TagsHaveAny(&a->requireTags,&b->forbidTags)){return qfalse;}
	if(G_TagsHaveAny(&b->requireTags,&a->forbidTags)){return qfalse;}
	for(i=0;i<a->numCriteria;i++){
		for(j=0;j<b->numCriteria;j++){
			if(a->criteria[i].key != b->criteria[j].key){continue;}
			if(!rangesOverlap(&a->criteria[i],&b->criteria[j])){return qfalse;}
		}
	}
	return qtrue;
}

static qboolean validateRules(const char *path){
	int i,j;
	for(i=0;i<ruleCount;i++){
		for(j=i+1;j<ruleCount;j++){
			if(rulesCanTie(&rules[i],&rules[j])){
				rulesError_f(path,rules[j].line,"rule '%s' can match the same state as rule '%s' with equal specificity",rules[j].name,rules[i].name);
				return qfalse;
			}
		}
	}
	for(i=0;i<ruleTestCount;i++){
		if(ruleTests[i].expect[0] && !G_RulesFind(ruleTests[i].expect)){
			rulesError_f(path,ruleTests[i].line,"test '%s' expects unknown rule '%s'",ruleTests[i].name,ruleTests[i].expect);
			return qfalse;
		}
	}
	return qtrue;
}

// ---------------------------------------------------------------- public API

// Called with the names loaded from masters.def, BEFORE content is parsed, so
// that `masterNear is rhogan` validates against the masters this server actually
// has. Deliberately not cleared by G_RulesReset: the vocabulary is loaded once
// per map and the rules are reloaded inside it.
void G_RulesSetMasterVocabulary(const char *const *names,int count){
	if(!names || count < 2){
		factDefs[fMasterNear].values = masterValues;
		factDefs[fMasterNear].numValues = ARRAY_LEN(masterValues);
		return;
	}
	factDefs[fMasterNear].values = names;
	factDefs[fMasterNear].numValues = count;
}

// Percent of the way from nothing to the goal, clamped. Quantizing here rather
// than at the call site is the plan's transport rule: what travels to the
// client is a percent, never an elapsed time, because a percent changes about a
// hundred times per lesson and milliseconds change every frame.
int G_RulesProgress(int value,int goal){
	if(goal <= 0 || value <= 0){return 0;}
	if(value >= goal){return 100;}
	return (int)(100.0f * value / goal);
}

void G_RulesReset(void){
	memset(tagNames,0,sizeof(tagNames));
	memset(tagGroups,0,sizeof(tagGroups));
	memset(g_worldFacts,0,sizeof(g_worldFacts));
	memset(&g_worldTags,0,sizeof(g_worldTags));
	tagCount = 0;
	tagGroupCount = 0;
	rules = NULL;
	ruleCount = 0;
	ruleTests = NULL;
	ruleTestCount = 0;
	rulesError[0] = 0;
}

// Counting pass. Storage is sized from it and allocated once, because G_Alloc
// is a bump allocator with no free and the content is fully known here.
static void countBlocks(char *text,int *outRules,int *outTests){
	rulesParse_t p;
	const char *token;
	int depth;

	memset(&p,0,sizeof(p));
	p.data = text;
	p.file = "";
	p.line = 1;
	depth = 0;
	*outRules = 0;
	*outTests = 0;
	while(1){
		token = rulesToken(&p,qfalse);
		if(!token[0]){break;}
		if(!Q_stricmp(token,"{")){depth++; continue;}
		if(!Q_stricmp(token,"}")){
			if(depth){depth--;}
			continue;
		}
		if(depth){continue;}
		if(!Q_stricmp(token,"rule")){(*outRules)++;}
		else if(!Q_stricmp(token,"test")){(*outTests)++;}
	}
}

qboolean G_RulesLoad(const char *tagsPath,const char *rulesPath){
	rulesParse_t p;
	const char *token;
	int length,wantRules,wantTests;

	G_RulesReset();
	if(!parseTagsDef(tagsPath)){return qfalse;}

	length = loadFile(rulesPath);
	if(length == -1){
		Com_sprintf(rulesError,sizeof(rulesError),"%s: rules file not found",rulesPath);
		G_Printf("%s\n",rulesError);
		return qfalse;
	}
	if(length == -2){
		Com_sprintf(rulesError,sizeof(rulesError),"%s: rules file larger than %i bytes",rulesPath,MAX_RULES_FILE);
		G_Printf("%s\n",rulesError);
		return qfalse;
	}

	countBlocks(fileBuffer,&wantRules,&wantTests);
	if(wantRules > MAX_RULES){
		Com_sprintf(rulesError,sizeof(rulesError),"%s: more than %i rules",rulesPath,MAX_RULES);
		G_Printf("%s\n",rulesError);
		return qfalse;
	}
	if(wantTests > MAX_RULE_TESTS){
		Com_sprintf(rulesError,sizeof(rulesError),"%s: more than %i tests",rulesPath,MAX_RULE_TESTS);
		G_Printf("%s\n",rulesError);
		return qfalse;
	}
	if(wantRules){rules = G_Alloc(wantRules * (int)sizeof(rule_t));}
	if(wantTests){ruleTests = G_Alloc(wantTests * (int)sizeof(ruleTest_t));}

	memset(&p,0,sizeof(p));
	p.data = fileBuffer;
	p.file = rulesPath;
	p.line = 1;
	while(1){
		token = rulesToken(&p,qfalse);
		if(!token[0]){break;}
		if(!Q_stricmp(token,"rule")){
			if(!parseRule(&p)){return qfalse;}
		}else if(!Q_stricmp(token,"test")){
			if(!parseTest(&p)){return qfalse;}
		}else{
			static const char *const blocks[] = {"rule","test"};
			const char *hint = suggest(token,blocks,2);
			if(hint){
				rulesError_f(p.file,p.tokenLine,"unknown declaration '%s' - did you mean '%s'?",token,hint);
			}else{
				rulesError_f(p.file,p.tokenLine,"unknown declaration '%s'",token);
			}
			return qfalse;
		}
	}
	return validateRules(rulesPath);
}

int G_RulesCount(void){
	return ruleCount;
}

const rule_t *G_RulesGet(int index){
	if(index < 0 || index >= ruleCount){return NULL;}
	return &rules[index];
}

const rule_t *G_RulesFind(const char *name){
	int i;
	for(i=0;i<ruleCount;i++){
		if(!Q_stricmp(rules[i].name,name)){return &rules[i];}
	}
	return NULL;
}

int G_RulesIndexOf(const rule_t *rule){
	if(!rule || !rules){return -1;}
	if(rule < rules || rule >= rules + ruleCount){return -1;}
	return (int)(rule - rules);
}

// ---------------------------------------------------------------- firing

// An accumulator counts unbroken time: it advances while its condition holds
// and drops to zero the moment it does not, because "airborne for 45 seconds"
// means 45 seconds without touching the ground, not 45 seconds in total.
int G_RulesAdvance(int accumulated,int msec,qboolean active){
	if(!active){return 0;}
	if(msec < 0){msec = 0;}
	if(accumulated < 0){accumulated = 0;}
	if(accumulated > MAX_QINT - msec){return MAX_QINT;}
	return accumulated + msec;
}

// Edge trigger for the evaluation loop. `latched` holds the best rule's index
// plus one, so a zeroed client starts with nothing latched rather than with
// rule 0. A rule's actions run on the frame it becomes the best match and not
// again while it stays one; the latch clears itself when the best match
// changes, including changing to no match at all.
//
// This is not what makes a rule fire once - content does that by granting a tag
// it also forbids, which stops the rule matching at all. The latch only stops a
// rule repeating while it remains the best match, which at sv_fps 20 would
// otherwise be twenty times a second.
qboolean G_RulesLatch(int *latched,int index){
	int want;
	want = index + 1;
	if(*latched == want){return qfalse;}
	*latched = want;
	return index >= 0 ? qtrue : qfalse;
}

int G_RulesTestCount(void){
	return ruleTestCount;
}

const ruleTest_t *G_RulesGetTest(int index){
	if(index < 0 || index >= ruleTestCount){return NULL;}
	return &ruleTests[index];
}

// ------------------------------------------------------------------ matching

static qboolean ruleMatches(const rule_t *rule,const int *clientFacts,const int *worldFacts,const tagSet_t *held){
	const criterion_t *criterion;
	int i,key,value;

	if(!G_TagsHaveAll(held,&rule->requireTags)){return qfalse;}
	if(G_TagsHaveAny(held,&rule->forbidTags)){return qfalse;}
	for(i=0;i<rule->numCriteria;i++){
		criterion = &rule->criteria[i];
		if(criterion->key & RULE_WORLD_KEY){
			key = criterion->key & ~RULE_WORLD_KEY;
			if(key < 0 || key >= fWorldFactCount){return qfalse;}
			value = worldFacts[key];
		}else{
			key = criterion->key;
			if(key < 0 || key >= fFactCount){return qfalse;}
			value = clientFacts[key];
		}
		if(value < criterion->min || value > criterion->max){return qfalse;}
	}
	return qtrue;
}

// Most criteria wins, which is what buys free fallbacks: author the specific
// and the generic side by side and the specific one takes over when it applies.
// Equal-specificity ties were rejected at load, so the scan cannot be ambiguous.
const rule_t *G_RulesMatch(const int *clientFacts,const int *worldFacts,const tagSet_t *clientTags,const tagSet_t *worldTags){
	const rule_t *best;
	tagSet_t held;
	int i,bestCount;

	memset(&held,0,sizeof(held));
	for(i=0;i<MAX_TAG_WORDS;i++){
		held.bits[i] = clientTags->bits[i] | worldTags->bits[i];
	}
	best = NULL;
	bestCount = -1;
	for(i=0;i<ruleCount;i++){
		if(rules[i].numCriteria <= bestCount){continue;}
		if(!ruleMatches(&rules[i],clientFacts,worldFacts,&held)){continue;}
		best = &rules[i];
		bestCount = rules[i].numCriteria;
	}
	return best;
}

// ------------------------------------------------------------ test execution

// A pure function of the loaded database, so authored vectors run under
// Criterion with ASan armed and again from a console command in game.
qboolean G_RulesRunTest(int index,char *err,int errSize){
	const ruleTest_t *test;
	const rule_t *matched;
	const char *got;

	test = G_RulesGetTest(index);
	if(!test){
		if(err && errSize){Com_sprintf(err,errSize,"no such test %i",index);}
		return qfalse;
	}
	matched = G_RulesMatch(test->facts,test->worldFacts,&test->tags,&test->worldTags);
	got = matched ? matched->name : "none";
	if(!Q_stricmp(got,test->expect[0] ? test->expect : "none")){
		if(err && errSize){err[0] = 0;}
		return qtrue;
	}
	if(err && errSize){
		Com_sprintf(err,errSize,"test '%s' expected '%s', matched '%s'",test->name,test->expect[0] ? test->expect : "none",got);
	}
	return qfalse;
}

qboolean G_RulesRunTests(int *passed,int *failed,char *err,int errSize){
	char text[MAX_RULE_ERROR];
	int i,pass,fail;

	pass = 0;
	fail = 0;
	if(err && errSize){err[0] = 0;}
	for(i=0;i<ruleTestCount;i++){
		if(G_RulesRunTest(i,text,sizeof(text))){
			pass++;
			continue;
		}
		fail++;
		if(err && errSize && !err[0]){Q_strncpyz(err,text,errSize);}
	}
	if(passed){*passed = pass;}
	if(failed){*failed = fail;}
	return fail ? qfalse : qtrue;
}
