#include "cg_local.h"

// The tournament ring, drawn: the tiled floor, the corner posts and the ki
// wall that brightens as the local fighter nears the boundary.
//
// The ring itself is server-side truth - g_ring.c owns the inside/outside test
// the round is decided by - and this file owns nothing but its appearance. It
// reads the SAME file the game module reads, rules/arena_<mapname>.def, exactly
// as cg_draw.c already reads rules/masters.def for the master vocabulary: the
// def files ship in GameData and are staged into the mod directory, so the
// client has them without a byte of new wire traffic.
//
// Two routes were open and the choice is worth stating, because the other one
// is the one the aura took.
//
//   A generated IQM registered as a model - the aura.iqm precedent - is right
//   when the mesh is fixed and the same on every map. This one is not: the
//   radius comes out of a per-map file an in-game editor writes, so a baked
//   mesh would have to be regenerated whenever an author moved the ring and
//   the build would have to know every map's radius in advance. It also could
//   not carry the wall's brightness, which is per-vertex and changes every
//   frame.
//
//   trap_R_AddPolyToScene builds the geometry from the numbers in the file at
//   the moment they are read, so arenaplace/arenasave round-trips visibly, and
//   the wall's proximity light is a vertex colour rather than a shader
//   parameter. That is the route taken. The cost is ~230 polys a frame against
//   a 16000-poly budget, all of them in one place and none of them clipped.
//
// The wall's brightness comes from cg.predictedPlayerState.origin - the local
// player's own distance to the boundary - so nothing about it is networked and
// a spectator sees the ring lit by whoever he is following.

#define ARENA_FILE_SIZE		2000

// Tessellation. The segment count is what makes the edge read as round; the
// band count is what lets the floor follow the ground it is draped on. Both are
// sized so that no floor quad spans more than about a fighter's width at the
// widest ring shipped (radius 448): 37 units radially, 58 along the rim. A quad
// wider than that cannot follow a slope its middle crosses, and the fighter
// standing on that middle is the one who ends up under it. 576 quads a frame
// against a 16000-poly budget.
#define ARENA_SEGMENTS		48
#define ARENA_FLOOR_BANDS	12

// One texture repeat per this many world units. Eight tiles to the sheet, so
// this is a 32-unit floor tile against a 56-unit fighter.
#define ARENA_TILE_UNITS	256.0f

// How far above the ground each floor vertex sits, and how far above and below
// the authored height the drape looks for that ground. The lift is small
// because fighters stand on the map's own surface rather than on this: every
// unit of it is a unit of boot buried, and it buys only the depth fight against
// the surface directly underneath.
#define ARENA_FLOOR_LIFT	1.0f
#define ARENA_DRAPE_UP		512.0f
#define ARENA_DRAPE_DOWN	1024.0f

// The ki wall: how tall it stands, and how far inside the boundary a fighter
// has to come before it starts answering him. The distance is deliberately
// wider than the corner spawn offset, so the wall is already alive when a round
// begins rather than switching on the first time somebody drifts.
#define ARENA_WALL_HEIGHT	320.0f
#define ARENA_WALL_NEAR		512.0f
#define ARENA_WALL_BASE		0.16f
// Two additive stages at full vertex brightness bleach everything behind them,
// so this is the ceiling the proximity term is scaled into: a wall a fighter
// reads through rather than one that hides his opponent.
#define ARENA_WALL_PEAK		0.55f

// Corner posts, at the ring edge on the two axes G_RingCorner works along, so
// the posts and the round's opening positions read off the same geometry.
#define ARENA_POST_HEIGHT	176.0f
#define ARENA_POST_WIDTH	40.0f

typedef struct {
	qboolean	valid;			// this map has an arena file that parsed
	qboolean	registered;		// shaders and sounds are in hand
	char		mapname[MAX_QPATH];
	vec3_t		center;
	float		radius;
	float		floor;
	qhandle_t	floorShader;
	qhandle_t	postShader;
	qhandle_t	wallShader;
	// The ring surface, draped onto the map once. floorZ[band][segment] is the
	// world height of the vertex at that radius and angle.
	float		floorZ[ARENA_FLOOR_BANDS+1][ARENA_SEGMENTS];
} cgArena_t;

static cgArena_t	arena;
static char		arenaBuffer[ARENA_FILE_SIZE+1];

/*================
CG_ArenaMapName

The per-map def files are named for the bare map - rules/arena_desert.def - and
cgs.mapname is the BSP path the client loaded, "maps/desert.bsp". The game
module gets the bare name straight from the mapname cvar; the client has to
take the path apart, and forgetting to is a ring that silently never loads.
================*/
void CG_ArenaMapName(char *out,int size){
	char	path[MAX_QPATH];

	Q_strncpyz(path,cgs.mapname,sizeof(path));
	COM_StripExtension(COM_SkipPath(path),out,size);
}

/*================
CG_ArenaLoad

Parsed the loose way on purpose. g_ring.c refuses a malformed file with a
file:line because a ring that silently fails to load takes the round rule with
it; here the worst case is a ring nobody can see, so anything that is not a
well-formed declaration is skipped rather than turned into an error the player
cannot act on.
================*/
static void CG_ArenaLoad(void){
	fileHandle_t	f;
	char			*token,*parse;
	int				length,i;
	vec3_t			center;
	float			radius,floor;

	memset(&arena,0,sizeof(arena));
	CG_ArenaMapName(arena.mapname,sizeof(arena.mapname));
	length = trap_FS_FOpenFile(va("rules/arena_%s.def",arena.mapname),&f,FS_READ);
	if(length <= 0){
		if(length == 0){trap_FS_FCloseFile(f);}
		return;
	}
	if(length > ARENA_FILE_SIZE){trap_FS_FCloseFile(f);return;}
	trap_FS_Read(arenaBuffer,length,f);
	arenaBuffer[length] = 0;
	trap_FS_FCloseFile(f);
	parse = arenaBuffer;
	while(1){
		token = COM_Parse(&parse);
		if(!token[0]){break;}
		if(Q_stricmp(token,"ring")){continue;}
		for(i=0;i<3;i++){center[i] = atof(COM_Parse(&parse));}
		radius = atof(COM_Parse(&parse));
		floor = atof(COM_Parse(&parse));
		if(radius <= 0){continue;}
		VectorCopy(center,arena.center);
		arena.radius = radius;
		arena.floor = floor;
		arena.valid = qtrue;
		break;
	}
}

/*================
CG_ArenaDrape

The arena file states ONE floor height, taken where the author stood, and the
ground under the rest of the ring is not at it - on desert the corners sit
tens of units lower, which a flat disc buries a fighter's boots in. So the ring
surface is traced onto the map instead, once, when the arena is registered:
~600 traces at load against 600 a frame, and a ring that is authored by
standing somewhere is drawn on the ground that is actually there.

The drape follows that ground wherever it goes, in both directions, and this is
the whole point: a fighter stands on the MAP's surface and never on this mesh,
so any unit by which the drawn floor departs from the ground under it is a unit
of him buried or a unit of him hovering. An earlier pass clamped the drape to
the authored height above and a step's worth below, on the theory that the ring
is a platform laid over the terrain - which is exactly what put a fighter's
legs through the floor on the ground that falls away past that step.

The authored height is therefore no longer a ceiling or a floor. It is only
where the trace starts looking, and the height a vertex keeps when there is
nothing under it at all - a ring hung over open air being the author's business
and not something to guess at.

Relief is reported so a placement can be judged: a ring wants flat ground, and
a drape that spans more than a fighter's height is a ring that should be moved
rather than a mesh that should be bent.
================*/
static void CG_ArenaDrape(void){
	trace_t	trace;
	vec3_t	start,end;
	float	r,a,ground,lo,hi;
	int		band,i,missed;

	lo = arena.floor;
	hi = arena.floor;
	missed = 0;
	for(band=0;band<=ARENA_FLOOR_BANDS;band++){
		r = arena.radius * band / (float)ARENA_FLOOR_BANDS;
		for(i=0;i<ARENA_SEGMENTS;i++){
			a = i * 2.0f * M_PI / ARENA_SEGMENTS;
			start[0] = end[0] = arena.center[0] + r*cos(a);
			start[1] = end[1] = arena.center[1] + r*sin(a);
			start[2] = arena.floor + ARENA_DRAPE_UP;
			end[2] = arena.floor - ARENA_DRAPE_DOWN;
			// The WORLD, not CG_Trace: an entity standing on the ring - a
			// fighter, a mover - would otherwise become part of its floor.
			trap_CM_BoxTrace(&trace,start,end,NULL,NULL,0,MASK_SOLID);
			if(trace.fraction < 1.0f && !trace.startsolid){
				ground = trace.endpos[2];
			}else{
				ground = arena.floor;
				missed++;
			}
			if(ground < lo){lo = ground;}
			if(ground > hi){hi = ground;}
			arena.floorZ[band][i] = ground + ARENA_FLOOR_LIFT;
		}
	}
	CG_Printf("Arena %s: floor draped, relief %.0f units (%.0f..%.0f about %.0f)%s\n",
		arena.mapname,hi-lo,lo-arena.floor,hi-arena.floor,arena.floor,
		missed ? va(", %d vertices over open air",missed) : "");
}

/*================
CG_ArenaRegister

Deferred to the first frame that wants the ring rather than done in
CG_RegisterGraphics, so a map with no arena file and a server that is not
running the tournament never touch the shaders at all.
================*/
static void CG_ArenaRegister(void){
	arena.registered = qtrue;
	arena.floorShader = trap_R_RegisterShader("ringFloorTile");
	arena.postShader = trap_R_RegisterShader("ringPost");
	arena.wallShader = trap_R_RegisterShader("ringKiWall");
	CG_ArenaDrape();
}

/*================
CG_ArenaActive

The ring is the tournament's furniture. It is drawn when this map has one and
the server is running GT_TOURNAMENT, and at no other time - a training session
on desert is not standing in the Budokai.
================*/
static qboolean CG_ArenaActive(void){
	char	mapname[MAX_QPATH];

	CG_ArenaMapName(mapname,sizeof(mapname));
	if(Q_stricmp(arena.mapname,mapname)){CG_ArenaLoad();}
	if(!arena.valid){return qfalse;}
	if(cgs.gametype != GT_TOURNAMENT){return qfalse;}
	if(!arena.registered){CG_ArenaRegister();}
	return qtrue;
}

// ------------------------------------------------------------------ drawing

static void CG_ArenaVert(polyVert_t *v,float x,float y,float z,float s,float t,int light){
	v->xyz[0] = x;
	v->xyz[1] = y;
	v->xyz[2] = z;
	v->st[0] = s;
	v->st[1] = t;
	v->modulate[0] = v->modulate[1] = v->modulate[2] = light;
	v->modulate[3] = 255;
}

/*================
CG_ArenaLight

One lightgrid sample for the whole ring, taken above the middle of it, rather
than CG_LightVerts per quad. Two reasons, and the second one is the important
one: the ring is one surface under one sky and the drape moves it by a fighter's
height at most, so a per-vertex sample buys nothing, and CG_LightVerts hands trap_R_LightForPoint an UNINITIALISED
ambientLight and uses it whatever the call returns. On a map with no lightgrid
- desert is one - that trap returns qfalse without writing a byte, and the ring
came out flat green. Defaults are set here before the call and kept if it
fails, so a map without a grid gets an unlit ring instead of a random one.
================*/
static int CG_ArenaLight(void){
	vec3_t	point,ambient,directed,dir;
	float	level;

	VectorSet(ambient,255,255,255);
	VectorSet(directed,0,0,0);
	VectorSet(dir,0,0,1);
	VectorCopy(arena.center,point);
	point[2] = arena.floor + 64;
	if(!trap_R_LightForPoint(point,ambient,directed,dir)){return 255;}
	// The floor faces up and the posts stand on it, so the directed term is
	// taken at the surface normal and the same value serves both.
	level = ambient[0] + directed[0] * (dir[2] > 0 ? dir[2] : 0);
	if(level > 255){level = 255;}
	if(level < 32){level = 32;}
	return (int)level;
}

/*================
CG_ArenaFloor

Planar texture coordinates taken from world x and y, so the tile grid is
continuous across every quad and the seams the tessellation introduces are
invisible. The innermost band's inner radius is zero, which collapses its quad
to a triangle - correct, and cheaper than special-casing a fan at the middle.
================*/
static void CG_ArenaFloorQuad(int band,int seg,int light){
	polyVert_t	verts[4];
	float		r[4],a[4],z[4],x,y;
	int			next,i;

	next = (seg+1) % ARENA_SEGMENTS;
	// Wound clockwise seen from above. This engine culls GL_FRONT rather than
	// GL_BACK (GL_Cull, tr_backend.c), so the counter-clockwise order that reads
	// as up-facing everywhere else is the one that disappears here.
	r[0] = arena.radius * band / (float)ARENA_FLOOR_BANDS;		a[0] = next;	z[0] = arena.floorZ[band][next];
	r[1] = arena.radius * (band+1) / (float)ARENA_FLOOR_BANDS;	a[1] = next;	z[1] = arena.floorZ[band+1][next];
	r[2] = r[1];							a[2] = seg;	z[2] = arena.floorZ[band+1][seg];
	r[3] = r[0];							a[3] = seg;	z[3] = arena.floorZ[band][seg];
	for(i=0;i<4;i++){
		x = arena.center[0] + r[i]*cos(a[i] * 2.0f * M_PI / ARENA_SEGMENTS);
		y = arena.center[1] + r[i]*sin(a[i] * 2.0f * M_PI / ARENA_SEGMENTS);
		CG_ArenaVert(&verts[i],x,y,z[i],x/ARENA_TILE_UNITS,y/ARENA_TILE_UNITS,light);
	}
	trap_R_AddPolyToScene(arena.floorShader,4,verts);
}

static void CG_ArenaFloor(int light){
	int	i,band;

	for(band=0;band<ARENA_FLOOR_BANDS;band++){
		for(i=0;i<ARENA_SEGMENTS;i++){
			CG_ArenaFloorQuad(band,i,light);
		}
	}
}

/*================
CG_ArenaWall

One quad per segment, standing on the boundary. The brightness is two terms
multiplied: how close the local fighter is to the edge at all, and how nearly
this segment is the piece of edge he is closest to. Together they read as a
wall that answers the fighter rather than one that pulses on a timer, and a
fighter in the middle of the ring sees the boundary as a hint instead of a box.
================*/
static void CG_ArenaWall(void){
	polyVert_t	verts[4];
	vec3_t		delta;
	float		a0,a1,z0,z1,dist,closeness,facing,bright,px,py,len;
	int			i,next,light;

	VectorSubtract(cg.predictedPlayerState.origin,arena.center,delta);
	delta[2] = 0;
	len = sqrt(delta[0]*delta[0] + delta[1]*delta[1]);
	dist = len - arena.radius;
	closeness = 1.0f + dist / ARENA_WALL_NEAR;
	if(closeness < 0){closeness = 0;}
	if(closeness > 1){closeness = 1;}
	// Direction to the fighter, so the lit arc follows him around. Dead centre
	// has no direction, and the whole wall comes up evenly instead.
	if(len > 1.0f){px = delta[0]/len;py = delta[1]/len;}
	else{px = 0;py = 0;}
	for(i=0;i<ARENA_SEGMENTS;i++){
		next = (i+1) % ARENA_SEGMENTS;
		z0 = arena.floorZ[ARENA_FLOOR_BANDS][i];
		z1 = arena.floorZ[ARENA_FLOOR_BANDS][next];
		a0 = i * 2.0f * M_PI / ARENA_SEGMENTS;
		a1 = (i+1) * 2.0f * M_PI / ARENA_SEGMENTS;
		facing = 0.5f * ((cos(a0)+cos(a1)) * px + (sin(a0)+sin(a1)) * py);
		if(facing < 0){facing = 0;}
		if(!px && !py){facing = 1;}
		bright = ARENA_WALL_PEAK * (ARENA_WALL_BASE + (1.0f - ARENA_WALL_BASE) * closeness * (0.30f + 0.70f * facing));
		if(bright > 1){bright = 1;}
		light = (int)(bright * 255);
		CG_ArenaVert(&verts[0],arena.center[0]+arena.radius*cos(a0),arena.center[1]+arena.radius*sin(a0),
			z0,i,1,light);
		CG_ArenaVert(&verts[1],arena.center[0]+arena.radius*cos(a1),arena.center[1]+arena.radius*sin(a1),
			z1,i+1,1,light);
		CG_ArenaVert(&verts[2],arena.center[0]+arena.radius*cos(a1),arena.center[1]+arena.radius*sin(a1),
			z1+ARENA_WALL_HEIGHT,i+1,0,light);
		CG_ArenaVert(&verts[3],arena.center[0]+arena.radius*cos(a0),arena.center[1]+arena.radius*sin(a0),
			z0+ARENA_WALL_HEIGHT,i,0,light);
		trap_R_AddPolyToScene(arena.wallShader,4,verts);
	}
}

/*================
CG_ArenaPosts

Four posts, on the boundary at the two axes G_RingCorner places fighters along.
Each is a pair of crossed quads rather than a model, for the same reason the
rest of the ring is polys: nothing about it survives a change of radius, and a
crossed pair reads as round from every angle a fighter sees it from.
================*/
static void CG_ArenaPosts(int light){
	polyVert_t	verts[4];
	float		yaw,cx,cy,dx,dy,z;
	int			i,pass,seg;

	for(i=0;i<4;i++){
		// The rim vertex nearest each quarter turn, so a post stands on the
		// same draped height as the piece of floor it is planted in.
		seg = (i * ARENA_SEGMENTS) / 4;
		yaw = seg * 2.0f * M_PI / ARENA_SEGMENTS;
		z = arena.floorZ[ARENA_FLOOR_BANDS][seg];
		cx = arena.center[0] + arena.radius * cos(yaw);
		cy = arena.center[1] + arena.radius * sin(yaw);
		for(pass=0;pass<2;pass++){
			// The two blades of the cross, at right angles to each other.
			dx = (pass ? -sin(yaw) : cos(yaw)) * ARENA_POST_WIDTH * 0.5f;
			dy = (pass ? cos(yaw) : sin(yaw)) * ARENA_POST_WIDTH * 0.5f;
			CG_ArenaVert(&verts[0],cx-dx,cy-dy,z,0,1,light);
			CG_ArenaVert(&verts[1],cx+dx,cy+dy,z,1,1,light);
			CG_ArenaVert(&verts[2],cx+dx,cy+dy,z+ARENA_POST_HEIGHT,1,0,light);
			CG_ArenaVert(&verts[3],cx-dx,cy-dy,z+ARENA_POST_HEIGHT,0,0,light);
			trap_R_AddPolyToScene(arena.postShader,4,verts);
		}
	}
}

/*================
CG_ArenaAmbience

The crowd bed, re-added every frame because that is what a looping sound IS in
this engine: S_AddLoopingSound builds the loop list from scratch per frame and a
sound not offered this frame stops. It is spatialised from the middle of the
ring on ENTITYNUM_WORLD rather than played flat, so flying out over the desert
takes the arena's noise with it - and no entity has to exist to carry it.
================*/
static void CG_ArenaAmbience(void){
	vec3_t	origin;

	if(!cgs.media.budokaiCrowdSound){return;}
	VectorCopy(arena.center,origin);
	origin[2] = arena.floor + 64;
	trap_S_AddLoopingSound(ENTITYNUM_WORLD,origin,vec3_origin,cgs.media.budokaiCrowdSound);
}

/*================
CG_ArenaCue

The three moments a round has, arriving as one reliable server command each
rather than as anything per-frame. The bell is a local sound on the announcer
channel: it is the tournament speaking to the player and not a noise in the
world, so it does not attenuate and it cannot be blocked by standing behind
something. The ring-out swell is on CHAN_AUTO underneath it, which is what
lets the two overlap instead of cutting each other off.
================*/
void CG_ArenaCue(int cue){
	switch(cue){
		case BDK_ROUND_START:
		case BDK_ROUND_OVER:
			trap_S_StartLocalSound(cgs.media.budokaiGongSound,CHAN_ANNOUNCER);
			break;
		case BDK_RING_OUT:
			trap_S_StartLocalSound(cgs.media.budokaiGongSound,CHAN_ANNOUNCER);
			trap_S_StartLocalSound(cgs.media.budokaiSwellSound,CHAN_AUTO);
			break;
		default:
			break;
	}
}

/*================
CG_AddArena

Called from the render list beside the other client-side scene builders, so the
ring is in the same pass as the marks and the particles and takes the same fog.
================*/
void CG_AddArena(void){
	int	light;

	if(!CG_ArenaActive()){return;}
	light = CG_ArenaLight();
	CG_ArenaFloor(light);
	CG_ArenaPosts(light);
	CG_ArenaWall();
	CG_ArenaAmbience();
}

/*================
CG_ArenaRing

Where the ring is, for the parts of cgame that need to stand something in it.
Null when this map has no arena or the tournament is not being played.
================*/
qboolean CG_ArenaRing(vec3_t center,float *radius,float *floor){
	if(!CG_ArenaActive()){return qfalse;}
	if(center){VectorCopy(arena.center,center);}
	if(radius){*radius = arena.radius;}
	if(floor){*floor = arena.floor;}
	return qtrue;
}
