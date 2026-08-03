#include "g_local.h"

// The tournament ring: the per-map arena file, the inside/outside maths and the
// ring-out decision.
//
// Written against the same failure mode as g_masters.c: a file that parses to
// nothing. A ring with no radius, or a second ring in one file, stops the load
// with a file:line rather than leaving a mode whose central rule never fires
// and nothing saying why.

static ring_t	ring;
static char	ringError[MAX_RING_ERROR];
static char	ringBuffer[MAX_RING_FILE];

// ------------------------------------------------------------- diagnostics

static void QDECL ringError_f(const char *file,int line,const char *fmt,...){
	va_list argptr;
	char text[MAX_RING_ERROR];

	// First error wins, as in g_rules.c and g_masters.c.
	if(ringError[0]){return;}
	va_start(argptr,fmt);
	Q_vsnprintf(text,sizeof(text),fmt,argptr);
	va_end(argptr);
	Com_sprintf(ringError,sizeof(ringError),"%s:%i: %s",file,line,text);
	G_Printf("%s\n",ringError);
}

const char *G_RingError(void){
	return ringError;
}

// ------------------------------------------------------------------ access

void G_RingReset(void){
	memset(&ring,0,sizeof(ring));
	ringError[0] = 0;
}

qboolean G_RingDefined(void){
	return ring.defined;
}

const ring_t *G_RingGet(void){
	return &ring;
}

// The ring is a cylinder with no lid: height is not part of being inside it,
// because a fighter hovering a hundred feet over the middle is still in the
// match and one standing on a rock past the edge is not.
float G_RingDistance(const vec3_t origin){
	float dx,dy;

	if(!ring.defined){return 0;}
	dx = origin[0] - ring.center[0];
	dy = origin[1] - ring.center[1];
	return sqrt(dx*dx + dy*dy) - ring.radius;
}

float G_RingHeight(const vec3_t origin){
	if(!ring.defined){return 0;}
	return origin[2] - ring.floor;
}

qboolean G_RingIsOut(const vec3_t origin,qboolean grounded){
	if(!ring.defined){return qfalse;}
	if(!grounded){return qfalse;}
	return G_RingDistance(origin) > 0 ? qtrue : qfalse;
}

void G_RingVantage(vec3_t origin,vec3_t angles){
	vec3_t look;

	VectorClear(origin);
	VectorClear(angles);
	if(!ring.defined){return;}
	origin[0] = ring.center[0] + ring.radius * RING_VANTAGE_BACK;
	origin[1] = ring.center[1];
	origin[2] = ring.floor + ring.radius * RING_VANTAGE_HEIGHT;
	look[0] = ring.center[0] - origin[0];
	look[1] = ring.center[1] - origin[1];
	look[2] = ring.floor - origin[2];
	vectoangles(look,angles);
}

void G_RingCorner(int index,vec3_t origin,vec3_t angles){
	vec3_t look;
	float side;

	VectorClear(origin);
	VectorClear(angles);
	if(!ring.defined){return;}
	side = (index & 1) ? -RING_CORNER_BACK : RING_CORNER_BACK;
	origin[0] = ring.center[0] + ring.radius * side;
	origin[1] = ring.center[1];
	// The same offset arenaplace took off when it wrote the floor: ps.origin is
	// the middle of the bounding box and the ring floor is where the feet go.
	origin[2] = ring.floor + RING_PLACE_FLOOR_DROP;
	look[0] = ring.center[0] - origin[0];
	look[1] = ring.center[1] - origin[1];
	look[2] = 0;
	vectoangles(look,angles);
}

// ----------------------------------------------------------------- parsing

typedef struct {
	char		*data;
	const char	*file;
	int		line;
	int		tokenLine;
} ringParse_t;

// COM_Parse skips whitespace and comments without reporting what it stepped
// over, so the line number is recovered by counting what the pointer passed.
static const char *ringToken(ringParse_t *p){
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

static qboolean ringNumber(ringParse_t *p,const char *what,float *out){
	const char *token;

	token = ringToken(p);
	if(!token[0] || (!isdigit((int)token[0]) && token[0] != '-' && token[0] != '+' && token[0] != '.')){
		ringError_f(p->file,p->tokenLine,"%s: expected a number, got '%s'",what,token);
		return qfalse;
	}
	*out = atof(token);
	return qtrue;
}

qboolean G_RingParse(char *text,const char *file){
	ringParse_t p;
	const char *token;
	vec3_t center;
	float radius,floor;
	int i;

	memset(&p,0,sizeof(p));
	p.data = text;
	p.file = file;
	p.line = 1;

	while(1){
		token = ringToken(&p);
		if(!token[0]){break;}
		if(Q_stricmp(token,"ring")){
			ringError_f(p.file,p.tokenLine,"unknown declaration '%s' - did you mean 'ring'?",token);
			return qfalse;
		}
		// A map has one ring. A second one is an authoring mistake rather than a
		// second arena, and silently keeping the last would hide it.
		if(ring.defined){
			ringError_f(p.file,p.tokenLine,"the ring is already defined at %.0f %.0f %.0f",
				ring.center[0],ring.center[1],ring.center[2]);
			return qfalse;
		}
		for(i=0;i<3;i++){
			if(!ringNumber(&p,"ring center",&center[i])){return qfalse;}
		}
		if(!ringNumber(&p,"ring radius",&radius)){return qfalse;}
		if(!ringNumber(&p,"ring floor",&floor)){return qfalse;}
		if(radius <= 0){
			ringError_f(p.file,p.tokenLine,"ring radius %.0f - a ring nobody can stand in",radius);
			return qfalse;
		}
		VectorCopy(center,ring.center);
		ring.radius = radius;
		ring.floor = floor;
		ring.defined = qtrue;
	}
	return qtrue;
}

// ----------------------------------------------------------------- loading

qboolean G_RingLoad(const char *path){
	fileHandle_t f;
	int length;

	length = trap_FS_FOpenFile(path,&f,FS_READ);
	// No file, no ring, no tournament arena on this map.
	if(length < 0){return qtrue;}
	if(length >= MAX_RING_FILE){
		trap_FS_FCloseFile(f);
		Com_sprintf(ringError,sizeof(ringError),"%s: arena file larger than %i bytes",path,MAX_RING_FILE);
		G_Printf("%s\n",ringError);
		return qfalse;
	}
	trap_FS_Read(ringBuffer,length,f);
	ringBuffer[length] = 0;
	trap_FS_FCloseFile(f);
	return G_RingParse(ringBuffer,path);
}

// --------------------------------------------------------------- authoring

qboolean G_RingPlace(const vec3_t origin,float radius,float floor){
	if(radius <= 0){
		Com_sprintf(ringError,sizeof(ringError),"a ring needs a radius above zero");
		return qfalse;
	}
	VectorCopy(origin,ring.center);
	ring.radius = radius;
	ring.floor = floor;
	ring.defined = qtrue;
	return qtrue;
}

qboolean G_RingWrite(const char *path,const char *mapname){
	fileHandle_t f;
	char line[512];

	if(!ring.defined){
		Com_sprintf(ringError,sizeof(ringError),"no ring is placed - arenaplace first");
		return qfalse;
	}
	trap_FS_FOpenFile(path,&f,FS_WRITE);
	if(!f){
		Com_sprintf(ringError,sizeof(ringError),"could not open %s for writing",path);
		G_Printf("%s\n",ringError);
		return qfalse;
	}
	Com_sprintf(line,sizeof(line),"// Tournament ring for %s, written by arenasave.\n"
		"//\n"
		"// One ring per map: center x y z, horizontal radius, then the world height\n"
		"// of the ring floor. A fighter who touches down past the radius loses the\n"
		"// round; flying out over the edge is legal.\n\n",mapname);
	trap_FS_Write(line,(int)strlen(line),f);
	Com_sprintf(line,sizeof(line),"ring  %.0f %.0f %.0f  %.0f  %.0f\n",
		ring.center[0],ring.center[1],ring.center[2],ring.radius,ring.floor);
	trap_FS_Write(line,(int)strlen(line),f);
	trap_FS_FCloseFile(f);
	G_Printf("arenasave: wrote the ring to %s\n",path);
	return qtrue;
}
