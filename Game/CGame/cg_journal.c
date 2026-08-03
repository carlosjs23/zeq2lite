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

// The status palette is the toast palette from cg_draw.c, for the same reason
// the tracker borrows the HUD's bar width: a completion is green here because a
// completion is green there.
static vec4_t	journalDone		= {0.588f,1.0f,0.0f,1.0f};
static vec4_t	journalAvailable	= {1.0f,0.85f,0.4f,1.0f};
static vec4_t	journalLocked		= {0.45f,0.50f,0.55f,0.8f};
static vec4_t	journalHeading		= {1.0f,1.0f,1.0f,0.9f};
static vec4_t	journalQuiet		= {0.7f,0.75f,0.8f,0.8f};
static vec4_t	journalBackdrop		= {0.0f,0.0f,0.0f,0.94f};

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

/*================
CG_JournalLine

The shadow pass exists for the same reason CG_DrawTrainingLine's does:
CG_DrawStringExt draws its own shadow at a fixed half alpha whatever colour it
is handed, so a dimmed row keeps a full-strength black outline and reads as
damaged rather than as quiet.
================*/
static void CG_JournalLine(int x,int y,const char *text,const vec4_t color){
	vec4_t	shadow = {0.0f,0.0f,0.0f,0.5f};

	shadow[3] = 0.5f * color[3];
	CG_DrawStringExt(-1,x+1,y+1,text,shadow,qtrue,qfalse,SMALLCHAR_WIDTH,SMALLCHAR_HEIGHT,0);
	CG_DrawStringExt(-1,x,y,text,color,qfalse,qfalse,SMALLCHAR_WIDTH,SMALLCHAR_HEIGHT,0);
}

/*================
CG_JournalLive

The only part of the page that is not a snapshot. persistant[] carries the
active objective and its percent every frame for free, so the line that moves
reads the snapshot and the list around it does not.
================*/
static void CG_JournalLive(void){
	const char	*text;
	int		active,progress;

	active = cg.snap->ps.persistant[PERS_TRAINING_OBJECTIVE];
	if(!active){
		CG_JournalLine(JOURNAL_LEFT,JOURNAL_LIVE_Y,"no objective in hand",journalQuiet);
		return;
	}
	progress = cg.snap->ps.persistant[PERS_TRAINING_PROGRESS];
	if(progress < 0){progress = 0;}
	if(progress > 100){progress = 100;}
	text = cg.trainingObjective[0] && cg.trainingObjectiveId == active ?
		va("now: %s  -  %i%%",cg.trainingObjective,progress) :
		va("now: training objective  -  %i%%",progress);
	CG_JournalLine(JOURNAL_LEFT,JOURNAL_LIVE_Y,text,journalAvailable);
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
			CG_JournalLine(JOURNAL_LEFT,y,text,journalHeading);
		}
		row++;
		for(l=0;l<section->count;l++){
			if(row - cg.journal.scroll >= JOURNAL_ROWS){return;}
			if(row < cg.journal.scroll){row++;continue;}
			lesson = &cg.journal.lessons[section->first + l];
			y = JOURNAL_BODY_Y + (row - cg.journal.scroll) * JOURNAL_ROW;
			// The glyph is what a player scans down the column, so it is a
			// character rather than a colour alone: colour blindness would
			// otherwise flatten three states into one.
			text = lesson->status == jrDone ? va("[x] %s",lesson->label) :
				lesson->status == jrAvailable ? va("[ ] %s",lesson->label) :
				va("[-] %s",lesson->label);
			CG_JournalLine(JOURNAL_LEFT+JOURNAL_INDENT,y,text,CG_JournalStatusColor(lesson->status));
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
	int		rows,width;

	if(!cg.journal.open){return;}
	CG_FillRect(0,0,SCREEN_WIDTH,SCREEN_HEIGHT,journalBackdrop);
	CG_JournalLine(JOURNAL_LEFT,JOURNAL_TITLE_Y,"TRAINING JOURNAL",journalHeading);
	// The key hint shares the title's line rather than sitting under the list.
	// A footer at the bottom of a 640x480 page lands on top of the status panel,
	// which is drawn first and owns that corner.
	rows = cg.journal.numSections + cg.journal.numLessons;
	hint = rows > JOURNAL_ROWS ? "escape closes    arrows scroll" : "escape closes";
	width = CG_DrawStrlen(hint) * SMALLCHAR_WIDTH / 2;
	CG_DrawStringExt(-1,JOURNAL_RIGHT-width,JOURNAL_TITLE_Y+SMALLCHAR_HEIGHT/4,hint,
		journalQuiet,qfalse,qfalse,SMALLCHAR_WIDTH/2,SMALLCHAR_HEIGHT/2,0);
	if(!cg.journal.valid){
		// A page that has asked and heard nothing says so. Drawing the empty
		// arrays instead would be a screen full of nothing that looks settled.
		CG_JournalLine(JOURNAL_LEFT,JOURNAL_HEAD_Y,"fetching...",journalQuiet);
		if(cg.journal.requestTime && cg.time - cg.journal.requestTime > JOURNAL_RETRY_TIME){
			CG_JournalRequest();
		}
		return;
	}
	CG_JournalLine(JOURNAL_LEFT,JOURNAL_HEAD_Y,
		va("tier ceiling %i    lessons %i of %i    tags held %i",
		cg.journal.tierCeiling,CG_JournalDoneCount(),cg.journal.numLessons,
		cg.journal.earnedTags),journalQuiet);
	CG_JournalLive();
	CG_JournalBody();
}
