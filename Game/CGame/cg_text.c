#include "cg_local.h"

// Proportional atlas type for the training surfaces.
//
// The stock charset is a 16x16 grid of fixed cells, so every string it draws is
// monospaced and the same weight. That is fine for a debug readout and it is
// the reason the training UI read as a debug readout: nothing in it could say
// "this word is the state, that sentence is the instruction". The approved
// treatment needs a heavy condensed display cut and a compact body cut, both
// proportional, so the metrics have to come from somewhere.
//
// They come from interface/training/training.font, generated next to the
// atlases by Tools/dev/make_training_font.py. Parsing a .def at load rather
// than compiling a generated table keeps the build order the same as every
// other generated asset here: the modules compile, then the art is made.
//
// The mechanism underneath is still one trap_R_DrawStretchPic per glyph,
// exactly like CG_DrawStringExt - the engine has no other 2D quad path. The
// difference is entirely in the numbers: each glyph carries its own cell,
// bearings and advance, so the pen walks by what the glyph is worth instead of
// by a constant.
//
// Everything here is virtual-space (640x480) and aspect-correct, the mapping
// the rest of the HUD uses. A training surface that stretched would drift away
// from the panel it sits beside as the display aspect changed.

#define	TEXT_FIRST_CHAR		32
#define	TEXT_LAST_CHAR		126
#define	TEXT_NUM_CHARS		(TEXT_LAST_CHAR-TEXT_FIRST_CHAR+1)
#define	TEXT_FILE			"interface/training/training.font"
#define	TEXT_FILE_SIZE		24000

typedef struct {
	short	x,y,w,h;		// cell in the atlas, in atlas pixels
	short	bx,by;			// pen-relative bearings; by is baseline-up to the top
	short	advance;
} textGlyph_t;

typedef struct {
	qhandle_t	shader;
	int			sheetWidth,sheetHeight;
	int			size;			// pixels per em the atlas was baked at
	int			ascent,descent,cap;
	// The deck sets the display faces in caps and bakes their gradient down the
	// cap band; a lowercase run through them would only fill the pale top of it.
	qboolean	caps;
	qboolean	valid;
	textGlyph_t	glyphs[TEXT_NUM_CHARS];
} cgFace_t;

static cgFace_t		cgTextFaces[TEXTFACE_COUNT];
// Static rather than automatic: the QVM's stack is a few kilobytes and this is
// twenty-four.
static char			cgTextBuffer[TEXT_FILE_SIZE+1];

/*================
CG_TextFaceByName
================*/
static int CG_TextFaceByName(const char *name){
	if(!Q_stricmp(name,"display")){return TEXTFACE_DISPLAY;}
	if(!Q_stricmp(name,"displayw")){return TEXTFACE_DISPLAYW;}
	if(!Q_stricmp(name,"body")){return TEXTFACE_BODY;}
	return -1;
}
/*================
CG_TextInit

Reads the generated metrics. A missing or malformed file leaves every face
invalid, which the draw calls treat as "draw nothing" rather than as a fault:
an install without the generated art should lose the training text, not the
frame it was going to be drawn on.
================*/
void CG_TextInit(void){
	fileHandle_t	f;
	cgFace_t		*face;
	textGlyph_t		*glyph;
	char			*token,*parse;
	int				length,index,code;

	memset(cgTextFaces,0,sizeof(cgTextFaces));
	length = trap_FS_FOpenFile(TEXT_FILE,&f,FS_READ);
	if(length <= 0 || length > TEXT_FILE_SIZE){
		if(length >= 0){trap_FS_FCloseFile(f);}
		CG_Printf("CG_TextInit: %s missing or oversized, training text disabled\n",TEXT_FILE);
		return;
	}
	trap_FS_Read(cgTextBuffer,length,f);
	cgTextBuffer[length] = 0;
	trap_FS_FCloseFile(f);
	parse = cgTextBuffer;
	face = NULL;
	while(1){
		token = COM_Parse(&parse);
		if(!token[0]){break;}
		if(!Q_stricmp(token,"face")){
			token = COM_Parse(&parse);
			index = CG_TextFaceByName(token);
			face = index < 0 ? NULL : &cgTextFaces[index];
			continue;
		}
		if(!face){continue;}
		if(!Q_stricmp(token,"end")){
			face->valid = qtrue;
			face = NULL;
			continue;
		}
		if(!Q_stricmp(token,"image")){
			face->shader = trap_R_RegisterShaderNoMip(COM_Parse(&parse));
			continue;
		}
		if(!Q_stricmp(token,"sheet")){
			face->sheetWidth = atoi(COM_Parse(&parse));
			face->sheetHeight = atoi(COM_Parse(&parse));
			continue;
		}
		if(!Q_stricmp(token,"size")){face->size = atoi(COM_Parse(&parse));continue;}
		if(!Q_stricmp(token,"ascent")){face->ascent = atoi(COM_Parse(&parse));continue;}
		if(!Q_stricmp(token,"descent")){face->descent = atoi(COM_Parse(&parse));continue;}
		if(!Q_stricmp(token,"cap")){face->cap = atoi(COM_Parse(&parse));continue;}
		if(!Q_stricmp(token,"caps")){face->caps = atoi(COM_Parse(&parse)) ? qtrue : qfalse;continue;}
		if(!Q_stricmp(token,"glyph")){
			code = atoi(COM_Parse(&parse));
			if(code < TEXT_FIRST_CHAR || code > TEXT_LAST_CHAR){
				// Still has to consume the row, or the parse desynchronises and
				// the rest of the face is read as garbage.
				for(index=0;index<7;index++){COM_Parse(&parse);}
				continue;
			}
			glyph = &face->glyphs[code-TEXT_FIRST_CHAR];
			glyph->x = atoi(COM_Parse(&parse));
			glyph->y = atoi(COM_Parse(&parse));
			glyph->w = atoi(COM_Parse(&parse));
			glyph->h = atoi(COM_Parse(&parse));
			glyph->bx = atoi(COM_Parse(&parse));
			glyph->by = atoi(COM_Parse(&parse));
			glyph->advance = atoi(COM_Parse(&parse));
		}
	}
	for(index=0;index<TEXTFACE_COUNT;index++){
		if(!cgTextFaces[index].valid || !cgTextFaces[index].size){
			cgTextFaces[index].valid = qfalse;
			CG_Printf("CG_TextInit: face %i incomplete\n",index);
		}
	}
}
/*================
CG_TextCaps

The deck's tracked label rows are set in caps as well, but the body face is not
a caps face - it also carries the sentences, which have to keep their case. So
the rows that are labels rather than prose uppercase what they were handed, and
that is a property of the row, not of the face.

One static buffer, like va(): a caller holding two of these at once is a caller
building one string out of two labels, which none of them do.
================*/
const char *CG_TextCaps(const char *text){
	static char	upper[128];
	int		i;

	Q_strncpyz(upper,text,sizeof(upper));
	for(i=0;upper[i];i++){
		if(upper[i] >= 'a' && upper[i] <= 'z'){upper[i] -= 'a' - 'A';}
	}
	return upper;
}
/*================
CG_TextValid
================*/
qboolean CG_TextValid(int faceIndex){
	if(faceIndex < 0 || faceIndex >= TEXTFACE_COUNT){return qfalse;}
	return cgTextFaces[faceIndex].valid;
}
/*================
CG_TextHeight

The line box the face wants at this em size. Callers place by the top of that
box, so a display line and a body line stacked together sit on predictable rows
whatever the two faces' own ascents happen to be.
================*/
float CG_TextHeight(int faceIndex,float size){
	const cgFace_t	*face;

	if(!CG_TextValid(faceIndex)){return size;}
	face = &cgTextFaces[faceIndex];
	return size * (float)(face->ascent + face->descent) / (float)face->size;
}
/*================
CG_TextWidth

`tracking` is in ems, matching the deck's letter-spacing: the caps rows are set
at .3em and the display type at none. It is added after every glyph including
the last, which is what CSS does and what keeps a right-aligned tracked string
from creeping one gap left of its neighbours.
================*/
float CG_TextWidth(int faceIndex,const char *text,float size,float tracking){
	const cgFace_t		*face;
	const textGlyph_t	*glyph;
	float				scale,width;
	int					c;

	if(!CG_TextValid(faceIndex) || !text){return 0;}
	face = &cgTextFaces[faceIndex];
	scale = size / (float)face->size;
	width = 0;
	while(*text){
		c = *(const unsigned char *)text++;
		if(c == '^' && *text && *text != '^'){text++;continue;}
		if(face->caps && c >= 'a' && c <= 'z'){c -= 'a' - 'A';}
		if(c < TEXT_FIRST_CHAR || c > TEXT_LAST_CHAR){continue;}
		glyph = &face->glyphs[c-TEXT_FIRST_CHAR];
		width += glyph->advance * scale + tracking * size;
	}
	return width;
}
/*================
CG_TextDraw

One stretched quad per glyph, colour multiplied through trap_R_SetColor. The
shadow is a second whole pass rather than a per-glyph pair so that a fading
string fades its shadow with it - the stock charset's fixed half-alpha shadow
is why a training toast used to end its life as a black smear.
================*/
void CG_TextDraw(int faceIndex,float x,float y,float size,const vec4_t color,
	const char *text,float tracking,int flags){
	const cgFace_t		*face;
	const textGlyph_t	*glyph;
	vec4_t				shadow;
	float				scale,pen,gx,gy,gw,gh,sw,sh;
	int					c;

	if(!CG_TextValid(faceIndex) || !text || !text[0]){return;}
	face = &cgTextFaces[faceIndex];
	scale = size / (float)face->size;
	if(flags & TEXTF_RIGHT){x -= CG_TextWidth(faceIndex,text,size,tracking);}
	else if(flags & TEXTF_CENTER){x -= 0.5f * CG_TextWidth(faceIndex,text,size,tracking);}
	if(flags & TEXTF_SHADOW){
		shadow[0] = shadow[1] = shadow[2] = 0;
		shadow[3] = 0.85f * color[3];
		CG_TextDraw(faceIndex,x+0.06f*size,y+0.07f*size,size,shadow,text,tracking,
			flags & ~(TEXTF_SHADOW|TEXTF_RIGHT|TEXTF_CENTER));
	}
	trap_R_SetColor(color);
	sw = (float)face->sheetWidth;
	sh = (float)face->sheetHeight;
	pen = x;
	while(*text){
		c = *(const unsigned char *)text++;
		if(c == '^' && *text && *text != '^'){text++;continue;}
		if(face->caps && c >= 'a' && c <= 'z'){c -= 'a' - 'A';}
		if(c < TEXT_FIRST_CHAR || c > TEXT_LAST_CHAR){continue;}
		glyph = &face->glyphs[c-TEXT_FIRST_CHAR];
		if(glyph->w > 0 && glyph->h > 0){
			gx = pen + glyph->bx * scale;
			gy = y + (face->ascent - glyph->by) * scale;
			gw = glyph->w * scale;
			gh = glyph->h * scale;
			CG_AdjustFrom640(&gx,&gy,&gw,&gh,qfalse);
			trap_R_DrawStretchPic(gx,gy,gw,gh,
				glyph->x/sw,glyph->y/sh,(glyph->x+glyph->w)/sw,(glyph->y+glyph->h)/sh,
				face->shader);
		}
		pen += glyph->advance * scale + tracking * size;
	}
	trap_R_SetColor(NULL);
}
/*================
CG_DrawShearedSlab

A slab cut at the deck's 20 degrees, at whatever width the text inside it came
out. Three quads: the triangular cap the generator baked, a plain middle, and
the same cap with its s coordinates reversed.

The cap has to be drawn tan(20)*height wide or it stops being a 20 degree cut -
it is a triangle in a square texture, so its angle is entirely a function of the
rectangle it is stretched into. That is why this exists instead of every caller
placing its own three quads.
================*/
void CG_DrawShearedSlab(float x,float y,float width,float height,const vec4_t color){
	float	cut,ax,ay,aw,ah;

	cut = height * TRAINING_SHEAR;
	if(width < 2.0f * cut){cut = 0.5f * width;}
	trap_R_SetColor(color);
	ax = x; ay = y; aw = cut; ah = height;
	CG_AdjustFrom640(&ax,&ay,&aw,&ah,qfalse);
	trap_R_DrawStretchPic(ax,ay,aw,ah,0,0,1,1,cgs.media.trainingCapShader);
	ax = x+cut; ay = y; aw = width-2.0f*cut; ah = height;
	CG_AdjustFrom640(&ax,&ay,&aw,&ah,qfalse);
	trap_R_DrawStretchPic(ax,ay,aw,ah,0,0,1,1,cgs.media.whiteShader);
	// Reversed in s, so the far end leans the same way the near end does.
	ax = x+width-cut; ay = y; aw = cut; ah = height;
	CG_AdjustFrom640(&ax,&ay,&aw,&ah,qfalse);
	trap_R_DrawStretchPic(ax,ay,aw,ah,1,0,0,1,cgs.media.trainingCapShader);
	trap_R_SetColor(NULL);
}
/*================
CG_DrawTrainingPic

CG_DrawPic with a colour, which the training art needs everywhere: the sash and
the plate fade out on the toast's own clock and the segments are tinted per
state, and CG_DrawPic leaves whatever colour the last caller set.
================*/
void CG_DrawTrainingPic(float x,float y,float width,float height,const vec4_t color,qhandle_t shader){
	trap_R_SetColor(color);
	CG_DrawPic(qfalse,x,y,width,height,shader);
	trap_R_SetColor(NULL);
}
/*================
CG_DrawTrainingPicST

The charge fill is a sub-rectangle of its texture rather than a squeezed copy of
it: squeezing would drag the ramp's bright end back to the pen every frame, so
a bar at 10% would be as bright at its tip as a bar at 90%.
================*/
void CG_DrawTrainingPicST(float x,float y,float width,float height,float s2,
	const vec4_t color,qhandle_t shader){
	CG_AdjustFrom640(&x,&y,&width,&height,qfalse);
	trap_R_SetColor(color);
	trap_R_DrawStretchPic(x,y,width,height,0,0,s2,1,shader);
	trap_R_SetColor(NULL);
}
