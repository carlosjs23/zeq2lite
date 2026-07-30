/*
An in-memory stand-in for the engine's trap_FS_* syscalls.

Parser tests are the highest-value unit tests in this codebase - every
join-time crash found so far has come from one - but the parsers reach the
filesystem through the VM syscall traps. Rather than write fixture files to disk
and depend on cwd, a suite declares its input inline:

    fake_fs_reset();
    fake_fs_add("music/playlist.cfg", "type random\nfade 0:04\n...");

and the linked trap_FS_* implementations serve it. Reads of unknown paths behave
like the real engine: length -1 and a zero handle.
*/

#ifndef FAKE_FS_H
#define FAKE_FS_H

/* Forget every registered file and close all handles. Call from zt_setup(). */
void fake_fs_reset(void);

/* Register a file. Content is copied. Overwrites a previous entry. */
void fake_fs_add(const char *path, const char *contents);

/* Register binary content (may contain NULs). */
void fake_fs_add_bytes(const char *path, const void *data, int length);

/* Diagnostics for assertions. */
int  fake_fs_open_count(void);   /* handles opened since reset */
int  fake_fs_leak_count(void);   /* handles opened but never closed */

#endif /* FAKE_FS_H */
