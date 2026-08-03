// Rule engine: facts, tags and the .rules script language.
//
// Kept free of g_local.h so the whole engine is a leaf unit - it reaches only
// q_shared, G_Alloc and the FS traps, which is what lets the Criterion suite
// link it without dragging the rest of the game module in.
#ifndef G_RULES_H
#define G_RULES_H

#include "../../Shared/q_shared.h"

// A prefix group is reserved whole at first declaration, so every tag sharing a
// prefix lands on contiguous bits and `requires trained.roshi.*` is one mask.
// The group size is therefore a hard per-prefix content limit, not just the
// global one.
#define MAX_TAG_BITS		512
#define TAG_GROUP_BITS		32
#define MAX_TAG_GROUPS		(MAX_TAG_BITS / TAG_GROUP_BITS)
#define MAX_TAG_WORDS		(MAX_TAG_BITS / 32)
#define MAX_TAG_NAME		64

#define MAX_RULE_NAME		32
#define MAX_RULE_CRITERIA	8
#define MAX_RULE_ACTIONS	8
#define MAX_RULES		512
#define MAX_RULE_TESTS		128
#define MAX_RULE_TEST_NAME	96
#define MAX_RULES_FILE		64000
#define MAX_RULE_ERROR		256
#define MAX_ACTION_TEXT		160

typedef enum {
	fPowerCurrent, fPowerPercent, fFatigue, fHealth,
	fTierCurrent, fTierTotal, fGravity, fStruggleEnergy,
	fPowerRaiseTime, fAirborneTime, fAuraTime, fMasterNear,
	fRingDistance, fRingHeight,
	fFactCount
} factKey_t;

// World facts are not attached to a player - round state, event timers, scores.
// Empty of content in Phase 1 and present anyway: retrofitting them would mean
// rewriting every rule, vector and authored file once content exists.
typedef enum {
	wRoundState, wRoundTime, wEventTimer, wScoreRed, wScoreBlue,
	fWorldFactCount
} worldFactKey_t;

// The named values of roundState, in the order its vocabulary declares them:
// `when world roundState is inProgress` compiles to roundInProgress, so the C
// that drives the fact and the content that reads it cannot drift apart.
typedef enum {
	roundWaiting, roundInProgress, roundOver
} roundState_t;

// Client and world facts share one criterion key space; the flag selects which
// array a criterion reads. Keeping criterion_t at three ints is the point of
// the range design and a second key field would undo it.
#define RULE_WORLD_KEY		0x100

typedef struct {
	unsigned int	bits[MAX_TAG_WORDS];
} tagSet_t;

typedef struct {
	int	key;
	int	min;
	int	max;
} criterion_t;

typedef enum {
	acGrant, acRemove, acSay, acObjective, acSetGravity, acUnlockTier,
	acActionCount
} actionType_t;

typedef struct {
	int	type;
	int	tag;		// acGrant, acRemove: tag bit
	char	*text;		// acSay, acObjective: pooled at load
	int	track;		// acObjective: fact key
	int	value;		// acObjective goal, acSetGravity, acUnlockTier
} action_t;

typedef struct {
	char		name[MAX_RULE_NAME];
	int		line;
	int		numCriteria;
	criterion_t	criteria[MAX_RULE_CRITERIA];
	tagSet_t	requireTags;
	tagSet_t	forbidTags;
	int		numActions;
	action_t	actions[MAX_RULE_ACTIONS];
} rule_t;

// A parsed `test { given ... expect ... }` block. Facts default to zero, so a
// vector states only what it means to constrain.
typedef struct {
	char		name[MAX_RULE_TEST_NAME];
	int		line;
	int		facts[fFactCount];
	int		worldFacts[fWorldFactCount];
	tagSet_t	tags;
	tagSet_t	worldTags;
	char		expect[MAX_RULE_NAME];	// empty means "expect no match"
} ruleTest_t;

extern int		g_worldFacts[fWorldFactCount];
extern tagSet_t		g_worldTags;

void		G_TagSet(tagSet_t *set,int bit);
void		G_TagClear(tagSet_t *set,int bit);
qboolean	G_TagTest(const tagSet_t *set,int bit);
qboolean	G_TagsHaveAll(const tagSet_t *set,const tagSet_t *need);
qboolean	G_TagsHaveAny(const tagSet_t *set,const tagSet_t *any);
qboolean	G_TagsEmpty(const tagSet_t *set);

int		G_TagFind(const char *name);
const char	*G_TagName(int bit);
int		G_TagCount(void);
// Contiguous mask for a declared prefix group, as `trained.roshi.*` compiles to.
qboolean	G_TagPrefixMask(const char *prefix,tagSet_t *out);

int		G_RulesFactKey(const char *name);
const char	*G_RulesFactName(int key);
// The declared unit and enum values behind a fact, so facts.def can be written
// out of the C enum rather than kept in step with it by hand.
const char	*G_RulesFactUnit(int key);
const char	*G_RulesFactValue(int key,int index);

// masterNear's named values come from rules/masters.def, so they are installed
// before content parses rather than compiled in. NULL restores the built-in
// table.
void		G_RulesSetMasterVocabulary(const char *const *names,int count);
int		G_RulesProgress(int value,int goal);

void		G_RulesReset(void);
qboolean	G_RulesLoad(const char *tagsPath,const char *rulesPath);
const char	*G_RulesError(void);

int		G_RulesCount(void);
const rule_t	*G_RulesGet(int index);
const rule_t	*G_RulesFind(const char *name);
int		G_RulesIndexOf(const rule_t *rule);

int		G_RulesAdvance(int accumulated,int msec,qboolean active);
qboolean	G_RulesLatch(int *latched,int index);

const rule_t	*G_RulesMatch(const int *clientFacts,const int *worldFacts,const tagSet_t *clientTags,const tagSet_t *worldTags);

int			G_RulesTestCount(void);
const ruleTest_t	*G_RulesGetTest(int index);
qboolean		G_RulesRunTest(int index,char *err,int errSize);
qboolean		G_RulesRunTests(int *passed,int *failed,char *err,int errSize);

#endif // G_RULES_H
