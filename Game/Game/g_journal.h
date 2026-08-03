// Journal: the loaded rule set read as a list of lessons, and the packing that
// carries that list to one client.
//
// Kept a leaf like g_rules.c and g_masters.c - it reaches q_shared and those two
// headers and nothing else - so the derivation the player actually reads (is
// this lesson done, available or locked) is unit testable without a running
// game. Everything that touches a client slot or sends a command lives in
// g_training.c.
#ifndef G_JOURNAL_H
#define G_JOURNAL_H

#include "../../Shared/q_shared.h"
#include "g_rules.h"

// One entry per lesson, and a lesson is a rule that grants something the player
// keeps. The shipped content has ten; the cap is the wire budget rather than a
// content limit, since a batch that will not fit in a handful of server commands
// is the thing MAX_RELIABLE_COMMANDS punishes.
#define MAX_JOURNAL_ENTRIES	64

// A label is a tag name, so the tag vocabulary's limit is the label's limit.
#define JOURNAL_LABEL		MAX_TAG_NAME

// Lessons of one master, packed into one command argument. Sized against the
// 1024-byte server command buffer with the command word, the master id and the
// quoted name still to fit around it.
#define JOURNAL_PACK_SIZE	700

// The separator between packed lessons. Labels are tag names and tier lines, so
// none of them can contain it; a label that somehow did would be split here, and
// the packer replaces it rather than letting that happen.
#define JOURNAL_SEPARATOR	'|'

// Solo drills - the lessons no master is standing next to - are a section like
// any other under this id, which is also masterNear's value for "nobody".
#define JOURNAL_SOLO_MASTER	0

typedef enum {
	jsLocked, jsAvailable, jsDone,
	jsStatusCount
} journalStatus_t;

typedef struct {
	char	label[JOURNAL_LABEL];
	int	rule;		// index of the rule this lesson is
	int	masterId;	// JOURNAL_SOLO_MASTER when no criterion names one
	int	status;		// journalStatus_t
} journalEntry_t;

// Builds the whole list against one client's tags and tier ceiling. Returns how
// many entries were written, in rule order, which is authoring order.
int	G_JournalBuild(const tagSet_t *tags,int unlockedTier,journalEntry_t *out,int maxOut);

// Status glyph, and its inverse. One char on the wire because the client only
// needs to pick a colour with it.
char	G_JournalStatusChar(int status);
int	G_JournalStatusFromChar(char c);

// Packs the entries of one master into `out` as
// "<statusChar><label>|<statusChar><label>|...", starting at *cursor and
// stopping when the buffer is full. Returns how many lessons were written and
// advances *cursor past them, so a master with more lessons than fit is sent as
// consecutive sections rather than truncated. The cursor belongs to one master:
// start it at zero per section and call again while it returns lessons.
int	G_JournalPack(const journalEntry_t *entries,int count,int masterId,int *cursor,char *out,int outSize);

#endif // G_JOURNAL_H
