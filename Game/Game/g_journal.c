#include "g_journal.h"

// The journal is a READING of the rule set, not a second copy of it. Nothing
// here is authored: a lesson is any rule that grants something the player keeps,
// its master is whichever master its criteria name, and its status falls out of
// the client's tags. Content that adds a lesson gets a journal line for free,
// and content that renames one cannot leave a stale entry behind, because there
// is nowhere for one to be stored.
//
// A lesson's label is the TAG IT GRANTS rather than its objective text or its
// say line. The objective text is the live line and is already on the HUD; the
// say line is a master's dialogue and reads as a result, not a title. The tag is
// what the lesson is worth and what "done" is decided on, so naming the entry
// after it makes the page and the save file say the same thing. A lesson that
// grants no tag - one that only opens a tier - is labelled by the tier.

// World tags belong to the round rather than to the player, so a rule that only
// grants one is not a lesson anybody can be done with. The prefix is the only
// marker the language has, and it is the same one g_training.c splits on.
#define JOURNAL_WORLD_PREFIX	"world."

/*================
lessonOf

What a rule leaves the player holding: the first non-world tag it grants, and
the highest tier it unlocks. A rule with neither is dialogue - a nudge, a
failure line - and is not a journal entry.
================*/
static qboolean lessonOf(const rule_t *rule,int *tagBit,int *tier){
	const char	*name;
	int		i;

	*tagBit = -1;
	*tier = 0;
	for(i=0;i<rule->numActions;i++){
		if(rule->actions[i].type == acGrant){
			if(*tagBit >= 0){continue;}
			name = G_TagName(rule->actions[i].tag);
			if(!Q_strncmp(name,JOURNAL_WORLD_PREFIX,(int)strlen(JOURNAL_WORLD_PREFIX))){continue;}
			*tagBit = rule->actions[i].tag;
			continue;
		}
		if(rule->actions[i].type == acUnlockTier && rule->actions[i].value > *tier){
			*tier = rule->actions[i].value;
		}
	}
	return (*tagBit >= 0 || *tier > 0) ? qtrue : qfalse;
}

/*================
masterOf

Which master's radius a lesson is taught in. Only an exact criterion counts -
`masterNear is roshi` compiles to [id,id] - because a range across several ids
would be a lesson that belongs to no one section.
================*/
static int masterOf(const rule_t *rule){
	int	i;

	for(i=0;i<rule->numCriteria;i++){
		if(rule->criteria[i].key != fMasterNear){continue;}
		if(rule->criteria[i].min != rule->criteria[i].max){continue;}
		if(rule->criteria[i].min <= 0){continue;}
		return rule->criteria[i].min;
	}
	return JOURNAL_SOLO_MASTER;
}

/*================
statusOf

Done is checked first and it has to be: a lesson forbids the tag it grants, so
the moment it is earned its own forbid set makes it read as unavailable. That is
the language working as designed, and the journal has to say "done" rather than
"locked" about it.
================*/
static int statusOf(const rule_t *rule,const tagSet_t *tags,int unlockedTier,int tagBit,int tier){
	if(tagBit >= 0){
		if(G_TagTest(tags,tagBit)){return jsDone;}
	}
	else if(tier > 0 && unlockedTier >= tier){
		return jsDone;
	}
	if(!G_TagsHaveAll(tags,&rule->requireTags)){return jsLocked;}
	if(G_TagsHaveAny(tags,&rule->forbidTags)){return jsLocked;}
	return jsAvailable;
}

int G_JournalBuild(const tagSet_t *tags,int unlockedTier,journalEntry_t *out,int maxOut){
	const rule_t	*rule;
	int		i,count,tagBit,tier;

	count = 0;
	for(i=0;i<G_RulesCount() && count < maxOut;i++){
		rule = G_RulesGet(i);
		if(!lessonOf(rule,&tagBit,&tier)){continue;}
		memset(&out[count],0,sizeof(out[count]));
		if(tagBit >= 0){
			Q_strncpyz(out[count].label,G_TagName(tagBit),sizeof(out[count].label));
		}
		else{
			Com_sprintf(out[count].label,sizeof(out[count].label),"tier %i",tier);
		}
		out[count].rule = i;
		out[count].masterId = masterOf(rule);
		out[count].status = statusOf(rule,tags,unlockedTier,tagBit,tier);
		count++;
	}
	return count;
}

char G_JournalStatusChar(int status){
	switch(status){
	case jsDone:		return 'd';
	case jsAvailable:	return 'a';
	}
	return 'l';
}

int G_JournalStatusFromChar(char c){
	switch(c){
	case 'd':	return jsDone;
	case 'a':	return jsAvailable;
	}
	return jsLocked;
}

int G_JournalPack(const journalEntry_t *entries,int count,int masterId,int *cursor,char *out,int outSize){
	const journalEntry_t	*entry;
	char			label[JOURNAL_LABEL];
	int			written,length,need,i;

	out[0] = 0;
	written = 0;
	length = 0;
	for(i=*cursor;i<count;i++){
		entry = &entries[i];
		if(entry->masterId != masterId){continue;}
		Q_strncpyz(label,entry->label,sizeof(label));
		// A label carrying the separator would arrive as two lessons, so it is
		// neutered here rather than trusted not to happen: tag names come from a
		// content file and the packer is the last place that can still see it.
		{
			char *scan;
			for(scan=label;*scan;scan++){
				if(*scan == JOURNAL_SEPARATOR){*scan = ' ';}
			}
		}
		// One status char, the label, and a separator unless this is the first.
		need = (written ? 1 : 0) + 1 + (int)strlen(label);
		if(length + need + 1 > outSize){break;}
		if(written){
			out[length++] = JOURNAL_SEPARATOR;
		}
		out[length++] = G_JournalStatusChar(entry->status);
		strcpy(out + length,label);
		length += (int)strlen(label);
		out[length] = 0;
		written++;
		// The cursor is what makes a second section continue where this one
		// stopped, so it advances past the entry rather than to it.
		*cursor = i + 1;
	}
	// Nothing matched at all: the caller has to be able to tell an empty section
	// from a full buffer, so the cursor is left where it was.
	return written;
}
