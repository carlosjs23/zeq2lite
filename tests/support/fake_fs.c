/*
In-memory implementation of the engine's trap_FS_* syscalls.

Linked into any suite that exercises a parser. See fake_fs.h for the rationale.
Behaviour is matched to Shared/files.c: a NULL handle pointer means "just report
the length without opening", and a miss yields length -1 with a zero handle.
*/

#include "fake_fs.h"

#include <stdlib.h>
#include <string.h>

#include "q_shared.h"

#define FAKE_FS_MAX_FILES   32
#define FAKE_FS_MAX_HANDLES 16

typedef struct {
	char  path[MAX_QPATH * 2];
	char *data;
	int   length;
} fake_file_t;

typedef struct {
	int in_use;
	int file;
	int offset;
} fake_handle_t;

static fake_file_t   fs_files[FAKE_FS_MAX_FILES];
static int           fs_file_count;
static fake_handle_t fs_handles[FAKE_FS_MAX_HANDLES];
static int           fs_opened;

void fake_fs_reset(void) {
	int i;
	for (i = 0; i < fs_file_count; ++i) {
		free(fs_files[i].data);
		fs_files[i].data = NULL;
	}
	fs_file_count = 0;
	memset(fs_handles, 0, sizeof(fs_handles));
	fs_opened = 0;
}

void fake_fs_add_bytes(const char *path, const void *data, int length) {
	fake_file_t *f = NULL;
	int i;

	for (i = 0; i < fs_file_count; ++i) {
		if (!Q_stricmp(fs_files[i].path, path)) {
			f = &fs_files[i];
			free(f->data);
			break;
		}
	}
	if (!f) {
		if (fs_file_count >= FAKE_FS_MAX_FILES) {
			return; /* suite bug; assertions will surface it */
		}
		f = &fs_files[fs_file_count++];
		Q_strncpyz(f->path, path, sizeof(f->path));
	}
	f->data = malloc((size_t)length + 1);
	memcpy(f->data, data, (size_t)length);
	f->data[length] = '\0';
	f->length = length;
}

void fake_fs_add(const char *path, const char *contents) {
	fake_fs_add_bytes(path, contents, (int)strlen(contents));
}

int fake_fs_open_count(void) {
	return fs_opened;
}

int fake_fs_leak_count(void) {
	int i, n = 0;
	for (i = 1; i < FAKE_FS_MAX_HANDLES; ++i) {
		if (fs_handles[i].in_use) {
			n++;
		}
	}
	return n;
}

/* ------------------------------------------------------------ the syscalls */

int trap_FS_FOpenFile(const char *qpath, fileHandle_t *f, fsMode_t mode) {
	int i, slot;

	(void)mode;
	for (i = 0; i < fs_file_count; ++i) {
		if (Q_stricmp(fs_files[i].path, qpath)) {
			continue;
		}
		/* files.c: a NULL handle means the caller only wants the length. */
		if (!f) {
			return fs_files[i].length;
		}
		for (slot = 1; slot < FAKE_FS_MAX_HANDLES; ++slot) {
			if (!fs_handles[slot].in_use) {
				fs_handles[slot].in_use = 1;
				fs_handles[slot].file = i;
				fs_handles[slot].offset = 0;
				fs_opened++;
				*f = slot;
				return fs_files[i].length;
			}
		}
		break; /* out of handles */
	}
	if (f) {
		*f = 0;
	}
	return -1;
}

void trap_FS_Read(void *buffer, int len, fileHandle_t f) {
	fake_handle_t *h;
	const fake_file_t *file;
	int avail;

	if (f <= 0 || f >= FAKE_FS_MAX_HANDLES || !fs_handles[f].in_use) {
		memset(buffer, 0, (size_t)len);
		return;
	}
	h = &fs_handles[f];
	file = &fs_files[h->file];
	avail = file->length - h->offset;
	if (avail < 0) {
		avail = 0;
	}
	if (len > avail) {
		/* Short read: zero the tail, as a real read would leave it untouched
		   but tests should not see indeterminate bytes. */
		memset((char *)buffer + avail, 0, (size_t)(len - avail));
		len = avail;
	}
	memcpy(buffer, file->data + h->offset, (size_t)len);
	h->offset += len;
}

void trap_FS_FCloseFile(fileHandle_t f) {
	if (f > 0 && f < FAKE_FS_MAX_HANDLES) {
		fs_handles[f].in_use = 0;
	}
}

void trap_FS_Write(const void *buffer, int len, fileHandle_t f) {
	(void)buffer;
	(void)len;
	(void)f;
}
