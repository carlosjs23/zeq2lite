/*
Shared/files.c - FS_CreatePath.

The bug this suite exists for: FS_CreatePath skipped the root directory with

    ofs = strchr( path, PATH_SEP );
    ofs++;

    for (; ofs != NULL && *ofs ; ofs++) {

On a path with no separator at all, strchr returns NULL, `ofs++` makes that 0x1
(undefined behaviour in its own right), and the ofs != NULL guard in the loop is
therefore useless - the loop dereferences 0x1 and the process dies.

Reaching it needed only `+set fs_basepath <anywhere>`: that makes fs_homepath
differ from fs_basepath, which is the condition under which FS_Startup calls
FS_CreatePath( fs_homepath->string ), and fs_homepath is a bare "." whenever the
engine is launched by a relative path such as ./ZEQ2.arm. So the standard Quake 3
way to point an engine at game data elsewhere was an instant segfault.

Sys_Mkdir is faked here so the tests can assert on which directories would be
created without touching the filesystem.
*/

#include <criterion/criterion.h>

#include "q_shared.h"
#include "qcommon.h"
#include "fake_mkdir.h"

/* ------------------------------------------------------------- the crash */

/*
A single component with no separator. This is the exact call FS_Startup makes
with a relative install path, and it segfaulted before the fix.
*/
Test(fs_path, single_component_does_not_crash) {
	char path[] = "ZEQ2";

	fake_mkdir_reset();
	FS_CreatePath( path );

	/* Nothing to create: the leaf itself is the caller's business. */
	cr_assert_eq( fake_mkdir_count(), 0,
	              "expected no mkdir for a bare component, got %d",
	              fake_mkdir_count() );
}

/* "." is what fs_homepath holds when argv[0] is ./ZEQ2.arm. */
Test(fs_path, current_directory_does_not_crash) {
	char path[] = ".";

	fake_mkdir_reset();
	FS_CreatePath( path );

	cr_assert_eq( fake_mkdir_count(), 0 );
}

Test(fs_path, empty_path_does_not_crash) {
	char path[] = "";

	fake_mkdir_reset();
	FS_CreatePath( path );

	cr_assert_eq( fake_mkdir_count(), 0 );
}

/* ------------------------------------------------- it still does its job */

/*
The point of the function: every parent directory gets created, in order, and the
leaf does not (callers pass a file path and want its containing directories).
*/
Test(fs_path, creates_each_parent_in_order) {
	char path[] = "/a/b/c/file.cfg";

	fake_mkdir_reset();
	FS_CreatePath( path );

	cr_assert_eq( fake_mkdir_count(), 3, "expected 3 mkdirs, got %d",
	              fake_mkdir_count() );
	cr_assert_str_eq( fake_mkdir_path( 0 ), "/a" );
	cr_assert_str_eq( fake_mkdir_path( 1 ), "/a/b" );
	cr_assert_str_eq( fake_mkdir_path( 2 ), "/a/b/c" );
}

/*
The first component is always skipped - the comment in the function calls it "the
root directory as it will always be there", which holds for the absolute paths
every real caller passes. On a relative path that same rule means the leading
component is assumed to exist, so "base" is not created here. Documented rather
than changed: creating directories a caller did not ask for is the riskier
behaviour of the two.
*/
Test(fs_path, relative_path_assumes_its_first_component_exists) {
	char path[] = "base/game/x.pk3";

	fake_mkdir_reset();
	FS_CreatePath( path );

	cr_assert_eq( fake_mkdir_count(), 1, "expected 1 mkdir, got %d",
	              fake_mkdir_count() );
	cr_assert_str_eq( fake_mkdir_path( 0 ), "base/game" );
}

/* The root always exists, so it is never created. */
Test(fs_path, root_alone_creates_nothing) {
	char path[] = "/";

	fake_mkdir_reset();
	FS_CreatePath( path );

	cr_assert_eq( fake_mkdir_count(), 0 );
}

/*
The separator is restored after each component, so the caller's buffer comes back
intact - FS_Startup passes fs_homepath->string straight in, and a truncated cvar
would break every later path built from it.
*/
Test(fs_path, caller_buffer_is_left_intact) {
	char path[] = "/a/b/c";

	fake_mkdir_reset();
	FS_CreatePath( path );

	cr_assert_str_eq( path, "/a/b/c" );
}

/* --------------------------------------------------------- path traversal */

Test(fs_path, refuses_dot_dot) {
	char path[] = "/a/../../etc/passwd";

	fake_mkdir_reset();
	cr_assert_eq( FS_CreatePath( path ), qtrue,
	              "a .. path must be refused" );
	cr_assert_eq( fake_mkdir_count(), 0, "refused path still created dirs" );
}
