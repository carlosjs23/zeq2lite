#include "cg_local.h"

// The masters, standing where their triggers are.
//
// A master is a place on a map before he is a character: g_masters.c owns the
// origin and the radius a lesson is keyed on, and until now there was nothing
// there to walk up to. This file puts the cast model on that spot.
//
// Route, and why it is not an entity. Three were open:
//
//   A client slot, the way g_dummy.c makes one. That is the only thing in this
//   tree that produces a full CG_Player assembly, and it is also a fighter as
//   far as the rest of the game is concerned - CalculateRanks counts it and
//   AddTournamentPlayer would pull a master into the fight line. A presence
//   must not be able to enter the tournament.
//
//   A new eType. entityType_t cannot take one: ET_EVENTS is a BASE index, with
//   every event entity riding above it, so anything inserted renumbers all of
//   them on a wire this fork keeps at protocol 71. Appending after ET_EVENTS is
//   not a value the client can distinguish from an event either.
//
//   ET_GENERAL with a modelindex, which is what a gib or a misc_model uses. It
//   draws one md3 with no skin, and a ZEQ2 character is three - lower, upper
//   and head, joined on tag_torso and tag_head, with one skin file over all
//   three. A single md3 there is an untextured pair of legs.
//
// So the master is drawn client-side from the placement file the game module
// reads, rules/masters_<map>.def, exactly as cg_arena.c draws the ring from the
// arena file. No entity, no client
// slot, no wire traffic, and the placement an author writes with masterplace is
// visible the moment the file is saved.
//
// The pose is the model's own ANIM_IDLE, run as a loop off cg.time. That is the
// bar this had to clear: a character standing in a T-pose reads as a broken
// asset rather than as a person, and the idle is already authored in every
// character's animation.cfg.

#define CG_MASTERS_MAX		16
#define CG_MASTERS_SIZE		8000

// The vocabulary's own name limit, restated rather than shared: g_masters.h is
// game-module code and cgame does not link it.
#define CG_MASTER_NAME		32

// Far enough that a master is a landmark, near enough that a map full of them
// would not each cost three models a frame. Comfortably past the widest master
// radius any placement file states.
#define CG_MASTER_DRAW_RANGE	4096.0f

// ps.origin is the middle of the bounding box and the placement is where the
// author was standing, so the model's feet go this far below the stated point -
// the same offset RING_PLACE_FLOOR_DROP takes off a ring floor.
#define CG_MASTER_FLOOR_DROP	24.0f

// How far a master will turn his head before his body has to come round with
// it. Seventy degrees is past what a neck does and well short of the point
// where the head reads as detached from the shoulders; the body then holds
// still for a player walking a full half-circle around him.
#define CG_MASTER_NECK_YAW		70.0f
#define CG_MASTER_NECK_PITCH	35.0f

typedef struct {
	char		name[CG_MASTER_NAME];
	vec3_t		origin;
	qhandle_t	iqmModel;	// whole character on one skeleton, when converted
	qhandle_t	legsModel;
	qhandle_t	torsoModel;
	qhandle_t	headModel;
	qhandle_t	skin;
	qboolean	drawable;
	// Where his body is pointed, kept between frames. A master turns his head
	// to follow the player and only swings his body when the head runs out of
	// travel, which needs the body's own facing remembered - it is no longer a
	// function of where the camera is this frame.
	float		bodyYaw;
	qboolean	bodyAimed;
} cgMaster_t;

static cgMaster_t	cgMasters[CG_MASTERS_MAX];
static int		cgMasterCount;
static char		cgMasterMap[MAX_QPATH];
static qboolean		cgMastersLoaded;
static char		cgMasterBuffer[CG_MASTERS_SIZE+1];

// The animation set is read with cgame's own animation.cfg parser, which wants
// a clientInfo_t to fill. One scratch is enough: only ANIM_IDLE is kept, and it
// is copied out before the next master is parsed. A clientInfo_t per master
// would carry eight tiers of icons, sounds and damage states for a character
// who never takes a hit.
static clientInfo_t	cgMasterScratch;
static animation_t	cgMasterIdle[CG_MASTERS_MAX];
static animation_t	cgMasterAnims[CG_MASTERS_MAX][MAX_TOTALANIMATIONS];

/*================
CG_MasterRegisterMD3

The three-part assembly, registered on demand. A master who converted does not
pay for these at load; cg_masterCompare pulls them in at the first frame that
asks for them, which costs one hitch on a control only a developer sets.
================*/
static qboolean CG_MasterRegisterMD3(cgMaster_t *master){
	char	path[MAX_QPATH];

	if(master->legsModel && master->torsoModel && master->headModel){return qtrue;}
	Com_sprintf(path,sizeof(path),"players/%s/tier1/lower.md3",master->name);
	master->legsModel = trap_R_RegisterModel(path);
	Com_sprintf(path,sizeof(path),"players/%s/tier1/upper.md3",master->name);
	master->torsoModel = trap_R_RegisterModel(path);
	Com_sprintf(path,sizeof(path),"players/%s/tier1/head.md3",master->name);
	master->headModel = trap_R_RegisterModel(path);
	return master->legsModel && master->torsoModel && master->headModel;
}

/*================
CG_MasterRegister

Tier 1 only. A master is met at the start of the arc, in the form the player
first sees him in, and a tier the content never transforms him into is a model
nobody loads.
================*/
static qboolean CG_MasterRegister(cgMaster_t *master){
	char	path[MAX_QPATH];

	// The skeletal build if the converter produced one, the three md3s if it
	// did not. Both are checked every time rather than the choice being made
	// once for the cast: a master whose IQM is missing is a master the build
	// skipped, and he should still be standing there.
	Com_sprintf(path,sizeof(path),"players/%s/tier1/character.iqm",master->name);
	master->iqmModel = trap_R_RegisterModel(path);
	Com_sprintf(path,sizeof(path),"players/%s/tier1/default.skin",master->name);
	master->skin = trap_R_RegisterSkin(path);
	// The md3 set is only loaded when it is going to be drawn. Three md3s are
	// thirteen megabytes against the IQM's three hundred kilobytes, so a
	// converted master that registered both would carry that cost for nothing.
	if(!master->iqmModel && !CG_MasterRegisterMD3(master)){return qfalse;}
	Com_sprintf(path,sizeof(path),"players/%s/animation.cfg",master->name);
	if(!CG_ParseAnimationFile(path,&cgMasterScratch,qtrue)){return qfalse;}
	// animation.cfg lands in camAnimations, not animations: CG_ParseAnimationFile
	// takes the player set as its 'isCamera' case and CG_SetLerpFrameAnimation reads
	// it back under the opposite flag, so the two inversions cancel everywhere in
	// cgame and reading the obvious field gets a set of zeroes.
	cgMasterIdle[master - cgMasters] = cgMasterScratch.camAnimations[ANIM_IDLE];
	if(cgMasterIdle[master - cgMasters].numFrames < 1){return qfalse;}
	// The whole set is kept only so cg_masterAnim can pick another one. It is
	// how the cost of the skeletal conversion is looked at rather than argued
	// about: the idle is rigid and converts exactly, and anything with a stride
	// in it does not.
	memcpy(cgMasterAnims[master - cgMasters],cgMasterScratch.camAnimations,
			sizeof(cgMasterAnims[0]));
	return qtrue;
}

/*================
CG_MastersLoad

The per-map placement file states who stands where. A master declared in the
vocabulary but not placed here is not drawn and is not an error - that is a map
he does not appear on, and this file never has to see the vocabulary to know it.

A model that will not register is dropped rather than substituted. A stand-in
character standing on another master's mark would be worse than an empty spot,
because the lesson that fires there names the master who is missing.
================*/
static void CG_MastersLoad(void){
	fileHandle_t	f;
	char			*token,*parse;
	char			mapname[MAX_QPATH];
	int				length,i;
	vec3_t			origin;

	cgMastersLoaded = qtrue;
	cgMasterCount = 0;
	memset(cgMasters,0,sizeof(cgMasters));
	CG_ArenaMapName(mapname,sizeof(mapname));
	Q_strncpyz(cgMasterMap,mapname,sizeof(cgMasterMap));
	length = trap_FS_FOpenFile(va("rules/masters_%s.def",mapname),&f,FS_READ);
	if(length <= 0){
		if(length == 0){trap_FS_FCloseFile(f);}
		return;
	}
	if(length > CG_MASTERS_SIZE){trap_FS_FCloseFile(f);return;}
	trap_FS_Read(cgMasterBuffer,length,f);
	cgMasterBuffer[length] = 0;
	trap_FS_FCloseFile(f);
	parse = cgMasterBuffer;
	while(cgMasterCount < CG_MASTERS_MAX){
		token = COM_Parse(&parse);
		if(!token[0]){break;}
		if(Q_stricmp(token,"place")){continue;}
		token = COM_Parse(&parse);
		if(!token[0]){break;}
		Q_strncpyz(cgMasters[cgMasterCount].name,token,sizeof(cgMasters[cgMasterCount].name));
		for(i=0;i<3;i++){origin[i] = atof(COM_Parse(&parse));}
		// The radius the game module gates the lesson on. Nothing here needs
		// it, but it has to come off the stream or the next 'place' is read as
		// a coordinate.
		COM_Parse(&parse);
		VectorCopy(origin,cgMasters[cgMasterCount].origin);
		cgMasters[cgMasterCount].origin[2] -= CG_MASTER_FLOOR_DROP;
		cgMasters[cgMasterCount].drawable = CG_MasterRegister(&cgMasters[cgMasterCount]);
		if(!cgMasters[cgMasterCount].drawable){
			CG_Printf("Masters: %s has no tier1 model, not drawn.\n",cgMasters[cgMasterCount].name);
		}
		cgMasterCount++;
	}
}

/*================
CG_MasterAnimate

The looping half of CG_RunLerpFrame, without the state machine around it: a
master never changes animation, so the frame pair is a function of cg.time and
nothing has to be carried between frames.
================*/
static void CG_MasterAnimate(const animation_t *anim,refEntity_t *ent){
	int		elapsed,frame;
	float	f;

	if(anim->frameLerp < 1){
		ent->oldframe = ent->frame = anim->firstFrame;
		ent->backlerp = 0;
		return;
	}
	elapsed = cg.time % (anim->numFrames * anim->frameLerp);
	frame = elapsed / anim->frameLerp;
	f = (elapsed % anim->frameLerp) / (float)anim->frameLerp;
	ent->oldframe = anim->firstFrame + frame;
	ent->frame = anim->firstFrame + (frame + 1) % anim->numFrames;
	ent->backlerp = 1.0f - f;
}

/*================
CG_MasterDraw

Legs carry the world orientation, the torso hangs off tag_torso and the head off
tag_head - the same chain CG_Player builds, minus everything a fighter needs and
a standing man does not.

He faces whoever is looking at him. A dummy does the same (g_dummy.c turns to
the nearest player), and it is the difference between a character waiting and a
statue: the greeting rule fires when the player lands in front of him, so the
model has to already be looking that way when it does.
================*/
static void CG_MasterDrawMD3(cgMaster_t *master,const animation_t *anim,
		const vec3_t origin,const vec3_t angles){
	refEntity_t	legs,torso,head;

	memset(&legs,0,sizeof(legs));
	memset(&torso,0,sizeof(torso));
	memset(&head,0,sizeof(head));

	AnglesToAxis(angles,legs.axis);
	VectorCopy(origin,legs.origin);
	VectorCopy(origin,legs.lightingOrigin);
	legs.hModel = master->legsModel;
	legs.customSkin = master->skin;
	legs.renderfx = RF_LIGHTING_ORIGIN;
	CG_MasterAnimate(anim,&legs);
	trap_R_AddRefEntityToScene(&legs);

	AxisClear(torso.axis);
	torso.hModel = master->torsoModel;
	torso.customSkin = master->skin;
	torso.renderfx = RF_LIGHTING_ORIGIN;
	VectorCopy(origin,torso.lightingOrigin);
	CG_MasterAnimate(anim,&torso);
	CG_PositionRotatedEntityOnTag(&torso,&legs,master->legsModel,"tag_torso");
	trap_R_AddRefEntityToScene(&torso);

	AxisClear(head.axis);
	head.hModel = master->headModel;
	head.customSkin = master->skin;
	head.renderfx = RF_LIGHTING_ORIGIN;
	VectorCopy(origin,head.lightingOrigin);
	CG_MasterAnimate(anim,&head);
	CG_PositionRotatedEntityOnTag(&head,&torso,master->torsoModel,"tag_head");
	trap_R_AddRefEntityToScene(&head);
}

/*================
CG_MasterDrawSkeletal

One entity for the whole character. The three parts are meshes on one skeleton
and the tag chain that used to be rebuilt here every frame is baked into the
pose track, so there is nothing to position: the frame pair alone puts the torso
over the hips and the head over the torso.

The headgear is a mesh weighted to its own bone under the head rather than
geometry merged into the head mesh, which is the thing this phase is for. It
follows the head for free and it is one bone away from being scaled, hidden or
aimed independently - none of which is reachable when the gear is welded into
the head's vertex list.

Frame numbers are the md3's own: the converter emits one IQM frame per md3
frame, so animation.cfg's ranges index the pose track directly and this shares
CG_MasterAnimate with the md3 path unchanged.
================*/
static void CG_MasterDrawSkeletal(cgMaster_t *master,const animation_t *anim,
		const vec3_t origin,const vec3_t angles,
		const refBone_t *bones,int numBones){
	refEntity_t	ent;

	memset(&ent,0,sizeof(ent));
	if(numBones > 0){
		if(numBones > REF_MAX_BONES){numBones = REF_MAX_BONES;}
		memcpy(ent.bones,bones,numBones * sizeof(ent.bones[0]));
		ent.numBones = numBones;
	}
	AnglesToAxis(angles,ent.axis);
	VectorCopy(origin,ent.origin);
	VectorCopy(origin,ent.lightingOrigin);
	ent.hModel = master->iqmModel;
	ent.customSkin = master->skin;
	ent.renderfx = RF_LIGHTING_ORIGIN;
	CG_MasterAnimate(anim,&ent);
	trap_R_AddRefEntityToScene(&ent);
}

/*================
CG_MasterDraw

He faces whoever is looking at him. A dummy does the same (g_dummy.c turns to
the nearest player), and it is the difference between a character waiting and a
statue: the greeting rule fires when the player lands in front of him, so the
model has to already be looking that way when it does.

cg_masterCompare puts the md3 assembly beside the skeletal one so the two can be
judged in the same frame under the same light. It is a developer control and not
a fallback - the choice of path is made by whether the IQM registered.
================*/
static void CG_MasterDraw(cgMaster_t *master,const animation_t *anim){
	vec3_t		delta,toView,angles,body,look,right,beside;
	refBone_t	bones[REF_MAX_BONES];
	int			numBones;
	float		swing;

	VectorSubtract(cg.refdef.vieworg,master->origin,delta);
	VectorCopy(delta,toView);
	delta[2] = 0;
	VectorClear(angles);
	if(delta[0] || delta[1]){vectoangles(delta,angles);}
	angles[PITCH] = angles[ROLL] = 0;

	if(!master->iqmModel){
		CG_MasterDrawMD3(master,anim,master->origin,angles);
		return;
	}

	// Head first, body second. The md3 path had no choice - a head is welded to
	// tag_head and the only thing that can be aimed is the whole assembly - so a
	// master pivoted on the spot to track anyone walking past him. With the head
	// as a bone the body can hold still and only give way when the neck runs
	// out of travel, which is what a person does.
	if(!master->bodyAimed){
		master->bodyYaw = angles[YAW];
		master->bodyAimed = qtrue;
	}
	swing = AngleSubtract(angles[YAW],master->bodyYaw);
	if(swing > CG_MASTER_NECK_YAW){master->bodyYaw = AngleMod(angles[YAW] - CG_MASTER_NECK_YAW);}
	else if(swing < -CG_MASTER_NECK_YAW){master->bodyYaw = AngleMod(angles[YAW] + CG_MASTER_NECK_YAW);}
	VectorClear(body);
	body[YAW] = master->bodyYaw;

	memset(bones,0,sizeof(bones));
	numBones = 0;
	VectorClear(look);
	look[YAW] = AngleSubtract(angles[YAW],master->bodyYaw);
	// Pitch is taken from the real line of sight rather than from the flattened
	// one, so a master looks up at a player hovering over him.
	if(toView[0] || toView[1]){
		look[PITCH] = -RAD2DEG(atan2(toView[2],sqrt(toView[0]*toView[0] + toView[1]*toView[1])));
		if(look[PITCH] > CG_MASTER_NECK_PITCH){look[PITCH] = CG_MASTER_NECK_PITCH;}
		if(look[PITCH] < -CG_MASTER_NECK_PITCH){look[PITCH] = -CG_MASTER_NECK_PITCH;}
	}
	// The angles land in the head bone's own frame, which on these rigs is the
	// model's frame: tag_head's axes are within half a degree of identity. On a
	// rig whose head tag carries a twist - the ones the README's donor-head note
	// is about - this would need that twist taken out first.
	Q_strncpyz(bones[0].name,"head",sizeof(bones[0].name));
	VectorCopy(look,bones[0].angles);
	numBones = 1;

	CG_MasterDrawSkeletal(master,anim,master->origin,body,bones,numBones);

	if(cg_masterCompare.value){
		AngleVectors(body,NULL,right,NULL);
		VectorMA(master->origin,cg_masterCompare.value,right,beside);
		if(cg_masterProportion.value){
			// The same character on the same skeleton with two bones scaled -
			// which is the thing a vertex-animated md3 cannot be asked for at
			// all, because its proportions are baked into every frame.
			memset(bones,0,sizeof(bones));
			Q_strncpyz(bones[0].name,"head",sizeof(bones[0].name));
			VectorCopy(look,bones[0].angles);
			VectorSet(bones[0].scale,cg_masterProportion.value,
					cg_masterProportion.value,cg_masterProportion.value);
			Q_strncpyz(bones[1].name,"lower",sizeof(bones[1].name));
			VectorSet(bones[1].scale,1.0f,1.0f,2.0f - cg_masterProportion.value);
			CG_MasterDrawSkeletal(master,anim,beside,body,bones,2);
		}else if(CG_MasterRegisterMD3(master)){
			CG_MasterDrawMD3(master,anim,beside,body);
		}
	}
}

/*================
CG_AddMasters

Called from the render list beside CG_AddArena. Range-culled rather than
frustum-culled: the renderer will reject a master behind the view anyway, and
the distance test is what keeps a map with a full roster from paying for
masters on the far side of it.
================*/
/*================
CG_MastersEnsure

The placement file is read on first use rather than at cgame init, because the
map it belongs to is not known until one is running. Every reader goes through
here so the radar and the render list cannot disagree about the roster.
================*/
static void CG_MastersEnsure(void){
	char	mapname[MAX_QPATH];

	CG_ArenaMapName(mapname,sizeof(mapname));
	if(!cgMastersLoaded || Q_stricmp(cgMasterMap,mapname)){CG_MastersLoad();}
}

/*================
CG_MasterCount / CG_MasterName / CG_MasterOrigin

The roster as the 2D layer needs it. A master with no model is still in it: a
mark on the radar is a place to fly to, and it stays true whether or not the
character standing there could be registered.
================*/
int CG_MasterCount(void){
	CG_MastersEnsure();
	return cgMasterCount;
}

const char *CG_MasterName(int index){
	CG_MastersEnsure();
	if(index < 0 || index >= cgMasterCount){return "";}
	return cgMasters[index].name;
}

const float *CG_MasterOrigin(int index){
	static vec3_t none = {0,0,0};

	CG_MastersEnsure();
	if(index < 0 || index >= cgMasterCount){return none;}
	return cgMasters[index].origin;
}

void CG_AddMasters(void){
	const animation_t	*anim;
	vec3_t				delta;
	int					i;

	CG_MastersEnsure();
	for(i=0;i<cgMasterCount;i++){
		if(!cgMasters[i].drawable){continue;}
		VectorSubtract(cgMasters[i].origin,cg.refdef.vieworg,delta);
		if(VectorLength(delta) > CG_MASTER_DRAW_RANGE){continue;}
		anim = &cgMasterIdle[i];
		if(cg_masterAnim.integer > 0 && cg_masterAnim.integer < MAX_TOTALANIMATIONS
		&& cgMasterAnims[i][cg_masterAnim.integer].numFrames > 0){
			anim = &cgMasterAnims[i][cg_masterAnim.integer];
		}
		CG_MasterDraw(&cgMasters[i],anim);
	}
}
