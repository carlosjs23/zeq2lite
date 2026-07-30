/*
Game/CGame/cg_music.c - duration parsing and the music playlist.

This is the unit whose stack-buffer overflow used to abort the game the instant a
player joined, so it gets the closest scrutiny. All input comes from the in-memory
fake filesystem (support/fake_fs.c), so the tests need no game data on disk.
*/

#include <criterion/criterion.h>

#include "cg_local.h"
#include "fake_fs.h"

static void music_setup(void) {
	fake_fs_reset();
	memset(&cgs.music, 0, sizeof(cgs.music));
	cg.time = 0;
}

TestSuite(cg_music, .init = music_setup);

/* ------------------------------------------------- CG_GetMilliseconds */

Test(cg_music, duration_mmss) {
	cr_assert_eq(CG_GetMilliseconds("0:04"), 4000);
	cr_assert_eq(CG_GetMilliseconds("2:21"), 141000);   /* 2*60+21 */
	cr_assert_eq(CG_GetMilliseconds("1:06"), 66000);
	cr_assert_eq(CG_GetMilliseconds("0:37"), 37000);
}

Test(cg_music, duration_bare_seconds) {
	cr_assert_eq(CG_GetMilliseconds("0"), 0);
	cr_assert_eq(CG_GetMilliseconds("7"), 7000);
	cr_assert_eq(CG_GetMilliseconds("90"), 90000);
}

Test(cg_music, duration_hhmmss) {
	cr_assert_eq(CG_GetMilliseconds("1:02:03"), (3600 + 120 + 3) * 1000);
}

/*
The regression that mattered. The old implementation copied the input into a
char[8] using an index that was only reset at a ':', so any token longer than
eight characters ran off the buffer. Because the playlist parser was also
mis-detecting its block delimiters, it fed track *names* here - 23 characters -
which smashed the stack canary and aborted the process with no diagnostic.

A duration parser has no business crashing on non-duration input, so that is
asserted directly: garbage in, zero out.
*/
Test(cg_music, long_non_duration_input_is_harmless) {
	cr_assert_eq(CG_GetMilliseconds("faulconer/nightmareEnds"), 0);
	cr_assert_eq(CG_GetMilliseconds("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 0);
	cr_assert_eq(CG_GetMilliseconds(""), 0);
}

Test(cg_music, duration_ignores_stray_characters) {
	/* CRLF-terminated .cfg files can leave a trailing \r on a token. */
	cr_assert_eq(CG_GetMilliseconds("2:21\r"), 141000);
}

Test(cg_music, duration_tolerates_null) {
	cr_assert_eq(CG_GetMilliseconds(NULL), 0);
}

/* ------------------------------------------------------ CG_ParsePlaylist */

static const char *PLAYLIST_TWO_TYPES =
	"type random\r\n"
	"fade 0:04\r\n"
	"battle{\r\n"
	"\tfaulconer/elite\t\t2:21\r\n"
	"\tfaulconer/rivals\t0:37\r\n"
	"}\r\n"
	"idle{\r\n"
	"\tfaulconer/scheme\t0:28\r\n"
	"}\r\n";

Test(cg_music, playlist_reads_global_settings) {
	fake_fs_add("music/playlist.cfg", PLAYLIST_TWO_TYPES);
	CG_ParsePlaylist();

	cr_assert_eq(cgs.music.random, qtrue);
	cr_assert_eq(cgs.music.fadeAmount, 4000);
	cr_assert_eq(cgs.music.started, qtrue);
}

/*
Block delimiters decide which type index every subsequent track lands in. The
original code tested them with strcmp() on the address of a single char, which
never matches under clang, so typeIndex stayed at -1 and every token - including
"battle{" - was treated as a track name.
*/
Test(cg_music, playlist_assigns_tracks_to_the_right_types) {
	fake_fs_add("music/playlist.cfg", PLAYLIST_TWO_TYPES);
	CG_ParsePlaylist();

	cr_assert_eq(cgs.music.typeSize[0], 2, "battle should hold 2 tracks");
	cr_assert_eq(cgs.music.typeSize[1], 1, "idle should hold 1 track");

	cr_assert_str_eq(cgs.music.playlist[0][0], "faulconer/elite");
	cr_assert_eq(cgs.music.trackLength[0][0], 141000);
	cr_assert_str_eq(cgs.music.playlist[0][1], "faulconer/rivals");
	cr_assert_eq(cgs.music.trackLength[0][1], 37000);
	cr_assert_str_eq(cgs.music.playlist[1][0], "faulconer/scheme");
	cr_assert_eq(cgs.music.trackLength[1][0], 28000);
}

Test(cg_music, playlist_initialises_last_track_markers) {
	int i;

	fake_fs_add("music/playlist.cfg", PLAYLIST_TWO_TYPES);
	CG_ParsePlaylist();

	/* -1 means "nothing played yet". The old code wrote this through
	   lastTrack[-1] before incrementing typeIndex, corrupting the struct. */
	for (i = 0; i < MUSICTYPES; ++i) {
		cr_assert_eq(cgs.music.lastTrack[i], -1, "type %d", i);
	}
}

Test(cg_music, playlist_closes_its_file_handle) {
	fake_fs_add("music/playlist.cfg", PLAYLIST_TWO_TYPES);
	CG_ParsePlaylist();

	cr_assert_eq(fake_fs_leak_count(), 0, "playlist.cfg handle was not closed");
}

Test(cg_music, missing_playlist_is_not_fatal) {
	/* Nothing registered in the fake filesystem. */
	CG_ParsePlaylist();

	cr_assert_eq(cgs.music.started, qtrue);
	cr_assert_eq(cgs.music.typeSize[0], 0);
	cr_assert_eq(fake_fs_leak_count(), 0);
}

Test(cg_music, playlist_survives_more_types_than_the_array_holds) {
	char big[4096];
	int i;

	big[0] = '\0';
	for (i = 0; i < MUSICTYPES + 6; ++i) {
		Q_strcat(big, sizeof(big), va("type%i{\n\ttrack%i\t0:10\n}\n", i, i));
	}
	fake_fs_add("music/playlist.cfg", big);

	CG_ParsePlaylist();  /* must clamp, not write past typeSize[MUSICTYPES-1] */

	cr_assert_eq(cgs.music.typeSize[MUSICTYPES - 1], 1);
}

Test(cg_music, playlist_survives_more_tracks_than_the_array_holds) {
	char big[8192];
	int i;

	Q_strncpyz(big, "battle{\n", sizeof(big));
	for (i = 0; i < MUSICTRACKS + 10; ++i) {
		Q_strcat(big, sizeof(big), va("\ttrack%i\t0:05\n", i));
	}
	Q_strcat(big, sizeof(big), "}\n");
	fake_fs_add("music/playlist.cfg", big);

	CG_ParsePlaylist();

	cr_assert_leq(cgs.music.typeSize[0], MUSICTRACKS,
	              "track count must be clamped to the array bound");
}

Test(cg_music, playlist_ignores_tracks_outside_a_block) {
	fake_fs_add("music/playlist.cfg", "orphan/track\t0:10\nbattle{\n\treal/track\t0:20\n}\n");

	CG_ParsePlaylist();  /* must not index typeSize[-1] */

	cr_assert_eq(cgs.music.typeSize[0], 1);
	cr_assert_str_eq(cgs.music.playlist[0][0], "real/track");
}

/* ------------------------------------------------------------ CG_NextTrack */

Test(cg_music, next_track_on_empty_type_does_nothing) {
	/* typeSize is all zero: there is no track to select and playlist[][] is
	   NULL, so a naive implementation would hand NULL to va(). */
	cgs.music.currentType = 0;
	CG_NextTrack();

	cr_assert_eq(cgs.music.endTime, 0);
}

Test(cg_music, next_track_advances_sequentially) {
	fake_fs_add("music/playlist.cfg", PLAYLIST_TWO_TYPES);
	CG_ParsePlaylist();

	cgs.music.random = qfalse;
	cgs.music.currentType = 0;
	cgs.music.currentIndex = -1;

	CG_NextTrack();
	cr_assert_eq(cgs.music.currentIndex, 0);
	CG_NextTrack();
	cr_assert_eq(cgs.music.currentIndex, 1,
	             "currentIndex must advance or the playlist replays track 0 forever");
}
