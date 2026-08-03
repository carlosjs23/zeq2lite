#include "cg_local.h"
#include "../../Engine/client/keycodes.h"

// The journal: the whole training progression on one page.
//
// It lives in cgame rather than in the UI module, and that is a data decision
// rather than a stylistic one. The UI VM receives no server commands at all, and
// the progression is per-server state held by the game module - so a UI menu
// would have nothing to draw. cgame gets the commands, and it can take input:
// CG_KEY_EVENT, CG_MOUSE_EVENT and CG_EVENT_HANDLING are dispatched at vmMain
// exactly like CG_DRAW_ACTIVE_FRAME is.
//
// Escape is not handled here and must not be. CL_KeyDownEvent treats it
// specially: it clears KEYCATCH_CGAME itself and then calls the module with
// CG_EVENT_HANDLING / CGAME_EVENT_NONE. Closing the page from that callback is
// what makes Escape work; a K_ESCAPE case in CG_JournalKey would never be
// reached, and taking the catcher back there would fight the engine for it.
//
// Two clocks, deliberately. The lesson list is a SNAPSHOT requested once when
// the page opens - the server sends a batch of reliable commands for it, and
// MAX_RELIABLE_COMMANDS is 64, so asking per frame would disconnect the client.
// The two numbers that move - the active objective and how far through it the
// player is - are read out of cg.snap every frame instead, because they are in
// persistant[] and cost nothing.
//
// This is a page, not a menu. There are no widgets, no focus and no mouse
// targets: the only inputs are scroll and close. Anything that needs a control
// belongs in the action vocabulary the rule engine already has.

// The status palette is the tracker's palette from cg_draw.c, for the same
// reason the tracker borrows the HUD's bar width: a completion is jade here
// because a completion is jade there.
//
// Each state is a tag block with a word in it rather than a colour alone. The
// word is what a player scans down the column, and it is what keeps the three
// states apart for the colour vision deficiencies that would otherwise flatten
// jade and blue into one.
static vec4_t	journalDone		= {0.475f,0.839f,0.651f,1.0f};
static vec4_t	journalAvailable	= {0.310f,0.639f,0.890f,1.0f};
static vec4_t	journalLocked		= {1.0f,1.0f,1.0f,0.16f};
static vec4_t	journalDoneInk		= {0.027f,0.153f,0.102f,1.0f};
static vec4_t	journalAvailableInk	= {0.016f,0.071f,0.122f,1.0f};
static vec4_t	journalLockedInk	= {1.0f,1.0f,1.0f,0.55f};
static vec4_t	journalSlab		= {0.549f,0.745f,0.941f,0.10f};
static vec4_t	journalSlabLocked	= {0.549f,0.745f,0.941f,0.045f};
static vec4_t	journalHeading		= {1.0f,1.0f,1.0f,1.0f};
static vec4_t	journalQuiet		= {1.0f,1.0f,1.0f,0.50f};
static vec4_t	journalDim		= {1.0f,1.0f,1.0f,0.34f};
static vec4_t	journalSashInk		= {0.165f,0.102f,0.024f,1.0f};
static vec4_t	journalBackdrop		= {0.027f,0.043f,0.082f,0.94f};

/*================
CG_JournalRequest

One request per open, and one retry if the batch never lands. The server rate
limits this as well, because a page that asked on every frame it was open would
spend the reliable command budget in three seconds.
================*/
static void CG_JournalRequest(void){
	cg.journal.requestTime = cg.time ? cg.time : 1;
	trap_SendClientCommand("journal");
}

void CG_JournalClose(void){
	if(!cg.journal.open){return;}
	cg.journal.open = qfalse;
	// Only ever drop the bit this page set. The engine clears it on Escape
	// before calling in here, so this has to be idempotent rather than a toggle.
	trap_Key_SetCatcher(trap_Key_GetCatcher() & ~KEYCATCH_CGAME);
}

void CG_JournalToggle(void){
	if(cg.journal.open){
		CG_JournalClose();
		return;
	}
	cg.journal.open = qtrue;
	cg.journal.scroll = 0;
	trap_Key_SetCatcher(trap_Key_GetCatcher() | KEYCATCH_CGAME);
	CG_JournalRequest();
}

/*================
CG_JournalKey

Scroll and nothing else. The page keeps its own clamp rather than clamping in
the draw, so a key held against the end of the list does not accumulate a scroll
value it has to walk back down.
================*/
void CG_JournalKey(int key){
	int	rows,limit;

	if(!cg.journal.open){return;}
	// Section headings and their lessons are one flat list of rows, which is
	// also what the draw walks.
	rows = cg.journal.numSections + cg.journal.numLessons;
	limit = rows - JOURNAL_ROWS;
	if(limit < 0){limit = 0;}
	switch(key){
	case K_UPARROW:
	case K_MWHEELUP:
		cg.journal.scroll--;
		break;
	case K_DOWNARROW:
	case K_MWHEELDOWN:
		cg.journal.scroll++;
		break;
	case K_PGUP:
		cg.journal.scroll -= JOURNAL_ROWS;
		break;
	case K_PGDN:
		cg.journal.scroll += JOURNAL_ROWS;
		break;
	case K_HOME:
		cg.journal.scroll = 0;
		break;
	case K_END:
		cg.journal.scroll = limit;
		break;
	}
	if(cg.journal.scroll > limit){cg.journal.scroll = limit;}
	if(cg.journal.scroll < 0){cg.journal.scroll = 0;}
}

// ------------------------------------------------------------- the batch

/*================
CG_JournalBegin

trjournal opens a batch and drops whatever was on the page. A client that was
looking at an older batch keeps drawing it until this arrives, so the page never
shows a half-replaced list.
================*/
void CG_JournalBegin(int tierCeiling,int earnedTags,int lessonTotal){
	cg.journal.receiving = qtrue;
	cg.journal.tierCeiling = tierCeiling;
	cg.journal.earnedTags = earnedTags;
	cg.journal.lessonTotal = lessonTotal;
	cg.journal.numSections = 0;
	cg.journal.numLessons = 0;
}

/*================
CG_JournalSection

One master's lessons, as "<statusChar><label>|<statusChar><label>|...". A master
with more lessons than one command holds arrives as consecutive sections with
the same id, so a repeat of the current id appends rather than opening a second
heading for the same name.

Anything that does not fit is dropped rather than wrapped into the next section:
the caps here are larger than the server's own, so overflowing them means the
content grew past what the transport was sized for and a silently reordered page
would be the worse failure.
================*/
void CG_JournalSection(int masterId,const char *name,const char *packed){
	journalSection_t	*section;
	journalLesson_t		*lesson;
	const char		*scan;
	int			length;

	if(!cg.journal.receiving){return;}
	section = NULL;
	if(cg.journal.numSections &&
		cg.journal.sections[cg.journal.numSections-1].masterId == masterId){
		section = &cg.journal.sections[cg.journal.numSections-1];
	}
	else if(cg.journal.numSections < MAX_JOURNAL_SECTIONS){
		section = &cg.journal.sections[cg.journal.numSections++];
		section->masterId = masterId;
		Q_strncpyz(section->name,name,sizeof(section->name));
		section->first = cg.journal.numLessons;
		section->count = 0;
	}
	if(!section){return;}
	scan = packed;
	while(*scan && cg.journal.numLessons < MAX_JOURNAL_LESSONS){
		lesson = &cg.journal.lessons[cg.journal.numLessons];
		// The first character of every field is the status; the rest is the
		// label, up to the separator or the end of the argument.
		switch(*scan){
		case 'd':	lesson->status = jrDone; break;
		case 'a':	lesson->status = jrAvailable; break;
		default:	lesson->status = jrLocked; break;
		}
		scan++;
		for(length=0;scan[length] && scan[length] != '|';length++){}
		if(length >= JOURNAL_LABEL_CHARS){length = JOURNAL_LABEL_CHARS-1;}
		memcpy(lesson->label,scan,length);
		lesson->label[length] = 0;
		cg.journal.numLessons++;
		section->count++;
		scan += length;
		while(*scan && *scan != '|'){scan++;}
		if(*scan == '|'){scan++;}
	}
}

void CG_JournalEnd(void){
	if(!cg.journal.receiving){return;}
	cg.journal.receiving = qfalse;
	cg.journal.valid = qtrue;
}

// ------------------------------------------------------------- the page

static int CG_JournalDoneCount(void){
	int	i,done;

	done = 0;
	for(i=0;i<cg.journal.numLessons;i++){
		if(cg.journal.lessons[i].status == jrDone){done++;}
	}
	return done;
}

static float *CG_JournalStatusColor(int status){
	switch(status){
	case jrDone:		return journalDone;
	case jrAvailable:	return journalAvailable;
	}
	return journalLocked;
}

static float *CG_JournalStatusInk(int status){
	switch(status){
	case jrDone:		return journalDoneInk;
	case jrAvailable:	return journalAvailableInk;
	}
	return journalLockedInk;
}

static const char *CG_JournalStatusWord(int status){
	switch(status){
	case jrDone:		return "DONE";
	case jrAvailable:	return "OPEN";
	}
	return "LOCK";
}

/*================
CG_JournalLive

The only part of the page that is not a snapshot. persistant[] carries the
active objective and its percent every frame for free, so the line that moves
reads the snapshot and the list around it does not.

It wears the tracker's gold sash, which is what makes "now" outrank every row
below it: the same object, in the same colour, saying the same thing.
================*/
static void CG_JournalLive(void){
	const char	*text;
	vec4_t		white = {1.0f,1.0f,1.0f,1.0f};
	int		active,progress;

	active = cg.snap->ps.persistant[PERS_TRAINING_OBJECTIVE];
	if(!active){
		CG_TextDraw(TEXTFACE_BODY,JOURNAL_LEFT,JOURNAL_LIVE_Y+6,TRAINING_BODY_SIZE,
			journalQuiet,"no objective in hand",0,TEXTF_SHADOW);
		return;
	}
	progress = cg.snap->ps.persistant[PERS_TRAINING_PROGRESS];
	if(progress < 0){progress = 0;}
	if(progress > 100){progress = 100;}
	CG_DrawTrainingPic(JOURNAL_LEFT,JOURNAL_LIVE_Y,JOURNAL_RIGHT-JOURNAL_LEFT,JOURNAL_NOW_H,
		white,cgs.media.trainingSashShader);
	CG_TextDraw(TEXTFACE_BODY,JOURNAL_LEFT+12,JOURNAL_LIVE_Y+7,TRAINING_CAPS_SIZE,
		journalSashInk,"NOW",TRAINING_CAPS_TRACK,0);
	text = cg.trainingObjective[0] && cg.trainingObjectiveId == active ?
		cg.trainingObjective : "training objective";
	CG_TextDraw(TEXTFACE_BODY,JOURNAL_LEFT+56,JOURNAL_LIVE_Y+5,TRAINING_BODY_SIZE,
		journalHeading,text,0,TEXTF_SHADOW);
	CG_TextDraw(TEXTFACE_DISPLAY,JOURNAL_RIGHT-12,JOURNAL_LIVE_Y+2,17,journalHeading,
		va("%i%%",progress),0,TEXTF_RIGHT|TEXTF_SHADOW);
}

/*================
CG_JournalBody

Sections and their lessons as one flat row list, so scrolling is an index rather
than a per-section calculation and a heading scrolls off the top like anything
else.
================*/
static void CG_JournalBody(void){
	const journalSection_t	*section;
	const journalLesson_t	*lesson;
	const char		*text;
	qboolean		locked;
	int			s,l,row,y;

	row = 0;
	for(s=0;s<cg.journal.numSections;s++){
		section = &cg.journal.sections[s];
		if(row - cg.journal.scroll >= JOURNAL_ROWS){return;}
		if(row >= cg.journal.scroll){
			y = JOURNAL_BODY_Y + (row - cg.journal.scroll) * JOURNAL_ROW;
			text = section->masterId && section->name[0] ?
				va("%s",section->name) :
				(section->masterId ? "a master" : "solo drills");
			CG_TextDraw(TEXTFACE_BODY,JOURNAL_LEFT,y+3,TRAINING_CAPS_SIZE,journalQuiet,
				CG_TextCaps(text),TRAINING_CAPS_TRACK,TEXTF_SHADOW);
		}
		row++;
		for(l=0;l<section->count;l++){
			if(row - cg.journal.scroll >= JOURNAL_ROWS){return;}
			if(row < cg.journal.scroll){row++;continue;}
			lesson = &cg.journal.lessons[section->first + l];
			y = JOURNAL_BODY_Y + (row - cg.journal.scroll) * JOURNAL_ROW;
			locked = lesson->status == jrLocked ? qtrue : qfalse;
			// Every row is the same sheared slab the tracker's plate is cut
			// from, and a locked one is that slab at a third of its fill: the
			// state is in the object as well as in the tag.
			CG_DrawShearedSlab(JOURNAL_LEFT+JOURNAL_INDENT,y,JOURNAL_SLAB_W,JOURNAL_SLAB_H,
				locked ? journalSlabLocked : journalSlab);
			CG_DrawShearedSlab(JOURNAL_LEFT+JOURNAL_INDENT+6,y+1,JOURNAL_TAG_W,JOURNAL_TAG_H,
				CG_JournalStatusColor(lesson->status));
			CG_TextDraw(TEXTFACE_BODY,JOURNAL_LEFT+JOURNAL_INDENT+13,y+2,7,
				CG_JournalStatusInk(lesson->status),CG_JournalStatusWord(lesson->status),
				0.16f,0);
			CG_TextDraw(TEXTFACE_BODY,JOURNAL_LEFT+JOURNAL_INDENT+JOURNAL_TAG_W+16,y+1,
				TRAINING_BODY_SIZE,locked ? journalDim : journalHeading,lesson->label,0,
				TEXTF_SHADOW);
			row++;
		}
	}
}

/*================
CG_DrawJournal

The backdrop is drawn STRETCHED because it has to cover every pixel of the
framebuffer; everything on top of it goes through the default aspect-correct
mapping, like the rest of the HUD. Mixing the two inside one element is the bug
CLAUDE.md's screen-space section is about.
================*/
void CG_DrawJournal(void){
	const char	*hint;
	int		rows;

	if(!cg.journal.open){return;}
	CG_FillRect(0,0,SCREEN_WIDTH,SCREEN_HEIGHT,journalBackdrop);
	CG_TextDraw(TEXTFACE_DISPLAY,JOURNAL_LEFT,JOURNAL_TITLE_Y,26,journalHeading,
		"TRAINING JOURNAL",0,TEXTF_SHADOW);
	// The key hint shares the title's line rather than sitting under the list.
	// A footer at the bottom of a 640x480 page lands on top of the status panel,
	// which is drawn first and owns that corner.
	rows = cg.journal.numSections + cg.journal.numLessons;
	hint = rows > JOURNAL_ROWS ? "escape closes    arrows scroll" : "escape closes";
	CG_TextDraw(TEXTFACE_BODY,JOURNAL_RIGHT,JOURNAL_TITLE_Y+10,TRAINING_CAPS_SIZE,journalDim,
		CG_TextCaps(hint),TRAINING_CAPS_TRACK,TEXTF_RIGHT|TEXTF_SHADOW);
	if(!cg.journal.valid){
		// A page that has asked and heard nothing says so. Drawing the empty
		// arrays instead would be a screen full of nothing that looks settled.
		CG_TextDraw(TEXTFACE_BODY,JOURNAL_LEFT,JOURNAL_HEAD_Y,TRAINING_CAPS_SIZE,journalQuiet,
			"FETCHING...",TRAINING_CAPS_TRACK,TEXTF_SHADOW);
		if(cg.journal.requestTime && cg.time - cg.journal.requestTime > JOURNAL_RETRY_TIME){
			CG_JournalRequest();
		}
		return;
	}
	CG_TextDraw(TEXTFACE_BODY,JOURNAL_LEFT,JOURNAL_HEAD_Y,TRAINING_CAPS_SIZE,journalAvailable,
		CG_TextCaps(va("tier ceiling %i   -   lessons %i of %i   -   tags held %i",
		cg.journal.tierCeiling,CG_JournalDoneCount(),cg.journal.numLessons,
		cg.journal.earnedTags)),TRAINING_CAPS_TRACK,TEXTF_SHADOW);
	CG_JournalLive();
	CG_JournalBody();
}
