#include "g_local.h"

// The save file itself. See g_progress.h for what a key is worth and why the
// tags are stored by name.
//
// The file is written and read by this module alone, so it carries no line
// numbers and no error text: a malformed entry is skipped, an unknown version
// refuses the whole file, and neither is a content mistake anybody can make by
// hand. That is the opposite of the .rules parser next door, which exists to
// tell an author exactly where they went wrong.

static char progressBuffer[MAX_PROGRESS_FILE];

// ------------------------------------------------------------------- keying

// One key character. Anything else is dropped rather than escaped, because the
// key becomes a filename: a key that can contain '/' or '.' is a key that can
// name a file outside the training directory.
static qboolean keyChar(char c,char *out){
	if(c >= 'A' && c <= 'Z'){*out = c - 'A' + 'a'; return qtrue;}
	if((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'){
		*out = c;
		return qtrue;
	}
	return qfalse;
}

static int keyAppend(const char *src,char *out,int outSize,int at){
	char c;
	while(*src && at < outSize - 1){
		if(keyChar(*src,&c)){out[at++] = c;}
		src++;
	}
	out[at] = 0;
	return at;
}

qboolean G_ProgressKey(const char *guid,const char *netname,char *out,int outSize){
	char clean[MAX_NETNAME];
	int at;

	out[0] = 0;
	if(guid && guid[0]){
		at = keyAppend(guid,out,outSize,0);
		if(at){return qtrue;}
	}
	// A listen server's own client usually has no guid at all, and that is the
	// single-player case - the way this mode is played most. Skipping
	// persistence there would mean the only player who never saves is the one
	// testing the content, so the cleaned name is the fallback. It is weaker
	// than a guid, but the guid was never strong (see the header), and a
	// listen-server host is already the person running the server.
	if(netname && netname[0]){
		Q_strncpyz(clean,netname,sizeof(clean));
		Q_CleanStr(clean);
		at = keyAppend("name_",out,outSize,0);
		at = keyAppend(clean,out,outSize,at);
		if(at > 5){return qtrue;}
	}
	// A name of nothing but colour codes and punctuation. Nothing to key on,
	// so this player does not persist and the caller says so once.
	out[0] = 0;
	return qfalse;
}

const char *G_ProgressPath(const char *key){
	return va("%s/%s%s",PROGRESS_DIR,key,PROGRESS_EXT);
}

// -------------------------------------------------------------- serializing

int G_ProgressTagCount(const progress_t *p){
	int i,count;
	count = 0;
	for(i=0;i<G_TagCount();i++){
		if(G_TagTest(&p->tags,i)){count++;}
	}
	return count;
}

int G_ProgressSerialize(const progress_t *p,const char *key,char *out,int outSize){
	char line[MAX_TAG_NAME + 32];
	int i,at,length;

	at = 0;
	Com_sprintf(line,sizeof(line),"// ZEQ2 training progress. Safe to delete.\n");
	length = (int)strlen(line);
	if(at + length >= outSize){return -1;}
	memcpy(out + at,line,length);
	at += length;

	Com_sprintf(line,sizeof(line),"version %i\nkey %s\ntier %i\n",
		PROGRESS_VERSION,key ? key : "",p->unlockedTier);
	length = (int)strlen(line);
	if(at + length >= outSize){return -1;}
	memcpy(out + at,line,length);
	at += length;

	for(i=0;i<G_TagCount();i++){
		if(!G_TagTest(&p->tags,i)){continue;}
		if(!G_TagName(i)[0]){continue;}
		Com_sprintf(line,sizeof(line),"tag %s\n",G_TagName(i));
		length = (int)strlen(line);
		if(at + length >= outSize){return -1;}
		memcpy(out + at,line,length);
		at += length;
	}
	out[at] = 0;
	return at;
}

static void reportDrop(progressLoad_t *report,const char *name){
	report->dropped++;
	if(report->named >= MAX_PROGRESS_DROPPED){return;}
	Q_strncpyz(report->droppedNames[report->named],name,MAX_TAG_NAME);
	report->named++;
}

qboolean G_ProgressParse(char *text,const char *file,progress_t *p,progressLoad_t *report){
	progressLoad_t local;
	const char *token;
	int bit;

	if(!report){report = &local;}
	memset(report,0,sizeof(*report));
	memset(p,0,sizeof(*p));

	while(1){
		token = COM_Parse(&text);
		if(!token[0]){break;}
		if(!Q_stricmp(token,"version")){
			report->version = atoi(COM_Parse(&text));
			if(report->version != PROGRESS_VERSION){
				G_Printf("training progress: %s is version %i, this build reads %i - not loaded.\n",
					file,report->version,PROGRESS_VERSION);
				memset(p,0,sizeof(*p));
				return qfalse;
			}
			continue;
		}
		// The version has to come first, so that a future format cannot be
		// half-read by this one before it reaches its own version line.
		if(!report->version){
			G_Printf("training progress: %s has no version line - not loaded.\n",file);
			memset(p,0,sizeof(*p));
			return qfalse;
		}
		if(!Q_stricmp(token,"tier")){
			p->unlockedTier = atoi(COM_Parse(&text));
			continue;
		}
		if(!Q_stricmp(token,"tag")){
			token = COM_Parse(&text);
			if(!token[0]){break;}
			bit = G_TagFind(token);
			// Content evolves and a renamed or retired tag is not an error -
			// it is a lesson that no longer exists. Dropping it silently would
			// be, so the caller gets the names back to log.
			if(bit < 0){
				reportDrop(report,token);
				continue;
			}
			G_TagSet(&p->tags,bit);
			report->restored++;
			continue;
		}
		// `key` and anything a later version adds: one value, skipped. Reading
		// past a key we do not know is how a file written by a newer build with
		// the same version number stays readable.
		COM_Parse(&text);
	}
	return qtrue;
}

// -------------------------------------------------------------------- files

// G_ProgressPath returns a va() buffer and the FS traps run va() themselves on
// the way through, so the path is copied into a local before it is handed over:
// a caller that holds a va() pointer across an open finds it clobbered.
qboolean G_ProgressRead(const char *key,progress_t *p,progressLoad_t *report){
	char path[MAX_QPATH];
	fileHandle_t f;
	int length;

	memset(p,0,sizeof(*p));
	if(report){memset(report,0,sizeof(*report));}
	Q_strncpyz(path,G_ProgressPath(key),sizeof(path));
	length = trap_FS_FOpenFile(path,&f,FS_READ);
	// No file is the normal case: it is what a player who has never trained
	// here looks like.
	if(length < 0 || !f){return qfalse;}
	if(length >= MAX_PROGRESS_FILE){
		trap_FS_FCloseFile(f);
		G_Printf("training progress: %s is larger than %i bytes - not loaded.\n",
			path,MAX_PROGRESS_FILE);
		return qfalse;
	}
	trap_FS_Read(progressBuffer,length,f);
	progressBuffer[length] = 0;
	trap_FS_FCloseFile(f);
	return G_ProgressParse(progressBuffer,path,p,report);
}

qboolean G_ProgressWrite(const char *key,const progress_t *p){
	char path[MAX_QPATH];
	fileHandle_t f;
	int length;

	Q_strncpyz(path,G_ProgressPath(key),sizeof(path));
	length = G_ProgressSerialize(p,key,progressBuffer,sizeof(progressBuffer));
	if(length < 0){
		G_Printf("training progress: %s would exceed %i bytes - not written.\n",
			path,MAX_PROGRESS_FILE);
		return qfalse;
	}
	trap_FS_FOpenFile(path,&f,FS_WRITE);
	if(!f){
		G_Printf("training progress: could not open %s for writing.\n",path);
		return qfalse;
	}
	trap_FS_Write(progressBuffer,length,f);
	trap_FS_FCloseFile(f);
	return qtrue;
}
