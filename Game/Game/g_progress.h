// Training progress: the per-player save file, the key it is named for, and
// the text format inside it.
//
// Kept a leaf like g_rules.c and g_masters.c - q_shared, g_rules.h and the FS
// traps - so the key derivation, the serializer and the parser are unit
// testable without a running game. Everything that touches a client slot, a
// userinfo string or games.log lives in g_training.c.
//
// WHO A SAVE FILE BELONGS TO, AND WHAT THAT IS WORTH
//
// Files are keyed on cl_guid, which the client sends in its userinfo. cl_guid
// is CVAR_USERINFO|CVAR_ROM, so ROM stops the STOCK client changing it, but
// userinfo is client-supplied text: a modified client sends whatever guid it
// likes and loads any save file it can name. This is trust-on-first-use and it
// is forgeable, deliberately, for v1.
//
// The plan (docs/training-mode-plan.md, "Persistence") states the fork:
// trust-on-first-use now - fine for a co-op community where progress is
// personal and nothing competitive rides on it - or a server-side name plus
// password, needed the moment progress gates anything a player would want to
// steal. This is the first branch. Take the second before progression gates
// anything but its own content.
//
// cl_guid is also per-server by default (cl_guidServerUniq), and an MD5 of a
// file on the player's own disk, so a save file follows a machine on one
// server. It is a key, never an identity.
#ifndef G_PROGRESS_H
#define G_PROGRESS_H

#include "../../Shared/q_shared.h"
#include "g_rules.h"

// Not .cfg: nothing here is engine configuration, and a .cfg under fs_game is
// something a player will eventually `exec`. The extension says so, and the
// file is written LF-only, unlike the CRLF configs beside it.
#define PROGRESS_DIR		"training"
#define PROGRESS_EXT		".progress"

// Bumped when a reader could misread an older file. An unknown version is
// refused whole rather than half-read, so a downgrade loses nothing it could
// have got right.
#define PROGRESS_VERSION	1

#define MAX_PROGRESS_KEY	48
#define MAX_PROGRESS_FILE	8000
// Dropped names are reported so the caller can log them; past this many the
// content has been reorganised and the count is the story, not the list.
#define MAX_PROGRESS_DROPPED	8

// What actually survives a disconnect. The active objective is not here: it
// re-fires from the rules the moment the facts warrant it, and a saved one
// would be a second source of truth for the same thing.
typedef struct {
	tagSet_t	tags;
	int		unlockedTier;
} progress_t;

typedef struct {
	int	version;
	int	restored;	// tag names that still resolve to a live bit
	int	dropped;	// names tags.def no longer declares
	int	named;		// entries filled in droppedNames
	char	droppedNames[MAX_PROGRESS_DROPPED][MAX_TAG_NAME];
} progressLoad_t;

// guid first, the cleaned netname as `name_<netname>` when the client has none
// (a listen server's own client often does). qfalse when neither yields a
// usable key, which is the "do not persist this player" answer.
qboolean	G_ProgressKey(const char *guid,const char *netname,char *out,int outSize);
const char	*G_ProgressPath(const char *key);

// The pure half, and what the suite exercises. Serialize returns the byte
// count written, or -1 if the buffer was too small; Parse takes a mutable
// buffer because COM_Parse walks one.
int		G_ProgressSerialize(const progress_t *p,const char *key,char *out,int outSize);
qboolean	G_ProgressParse(char *text,const char *file,progress_t *p,progressLoad_t *report);

// Tags are stored by NAME. Bit positions move whenever tags.def gains a tag in
// the middle of a prefix group, and a save file that meant bit 37 would then
// restore whatever now lives there. Names only ever disappear.
int		G_ProgressTagCount(const progress_t *p);

qboolean	G_ProgressRead(const char *key,progress_t *p,progressLoad_t *report);
qboolean	G_ProgressWrite(const char *key,const progress_t *p);

#endif // G_PROGRESS_H
