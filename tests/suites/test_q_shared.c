/*
Shared/q_shared.c - string and tokenising primitives.

These are the most-reused functions in the codebase and the ones whose exact
semantics have already caused shipped crashes, so they are pinned down here
first. In particular Q_strncpyz's padding behaviour (see below) is the direct
cause of the cg_weapGfxBuffer.weaponName overflow: it is not obvious from the
name, so it is documented as an executable assertion rather than a comment.
*/

#include <criterion/criterion.h>

#include "q_shared.h"

/* ------------------------------------------------------------- Q_strncpyz */

Test(q_shared, strncpyz_copies_short_string) {
	char dst[16];

	Q_strncpyz(dst, "goku", sizeof(dst));
	cr_assert_str_eq(dst, "goku");
}

Test(q_shared, strncpyz_truncates_to_destsize_minus_one) {
	char dst[8];

	Q_strncpyz(dst, "abcdefghijkl", sizeof(dst));
	cr_assert_str_eq(dst, "abcdefg");
	cr_assert_eq(strlen(dst), sizeof(dst) - 1);
}

Test(q_shared, strncpyz_always_terminates) {
	char dst[4];

	Q_strncpyz(dst, "xyz!", sizeof(dst));
	cr_assert_eq(dst[sizeof(dst) - 1], '\0');
}

/*
The behaviour that matters, and the reason a too-large destsize is not merely
sloppy but actively corrupting: Q_strncpyz wraps strncpy, which PADS the
destination with NULs out to n bytes. So the write is always destsize bytes wide
regardless of how short the source is.

Passing sizeof() of some *other, larger* field therefore overflows on every
call - even when copying "". That is exactly what
Game/CGame/cg_weapGfxParser.c did into weaponName[40] with a 64-byte bound.
*/
Test(q_shared, strncpyz_pads_the_whole_destination) {
	char buf[32];
	int i;

	memset(buf, 'X', sizeof(buf));
	Q_strncpyz(buf, "hi", 16);

	cr_assert_str_eq(buf, "hi");
	for (i = 2; i < 16; ++i) {
		cr_assert_eq(buf[i], '\0',
		             "byte %d should have been zero-padded by strncpy", i);
	}
	/* Anything past destsize must be untouched - that is the boundary a wrong
	   bound crosses. */
	for (i = 16; i < 32; ++i) {
		cr_assert_eq(buf[i], 'X', "byte %d is outside destsize and must not change", i);
	}
}

/* ----------------------------------------------------------- Q_PrintStrlen */

Test(q_shared, printstrlen_ignores_colour_codes) {
	cr_assert_eq(Q_PrintStrlen("abc"), 3);
	cr_assert_eq(Q_PrintStrlen("^1abc"), 3);
	cr_assert_eq(Q_PrintStrlen("^1a^2b^3c"), 3);
}

/*
Q_PrintStrlen counts printable characters, not bytes. Using it as a byte index
bound - as the old CG_GetMilliseconds did - silently walks off the end of a
string that contains colour codes.
*/
Test(q_shared, printstrlen_is_not_a_byte_length) {
	const char *s = "^1abc";

	cr_assert_neq(Q_PrintStrlen(s), (int)strlen(s),
	              "colour-coded strings must expose the byte/print divergence");
}

/* --------------------------------------------------------------- Q_stricmp */

Test(q_shared, stricmp_is_case_insensitive) {
	cr_assert_eq(Q_stricmp("Battle", "battle"), 0);
	cr_assert_neq(Q_stricmp("battle", "idle"), 0);
}

/* --------------------------------------------------------------- COM_Parse */

Test(q_shared, com_parse_splits_on_whitespace) {
	char  text[] = "type random";
	char *p = text;

	cr_assert_str_eq(COM_Parse(&p), "type");
	cr_assert_str_eq(COM_Parse(&p), "random");
	cr_assert_str_eq(COM_Parse(&p), "");
}

/*
The stock playlist.cfg and several other shipped .cfg files use CRLF line
endings. COM_Parse must treat \r as whitespace, or every token on a line would
carry a trailing carriage return and no string comparison would ever match.
*/
Test(q_shared, com_parse_treats_crlf_as_whitespace) {
	char  text[] = "fade\r\n0:04\r\n";
	char *p = text;

	cr_assert_str_eq(COM_Parse(&p), "fade");
	cr_assert_str_eq(COM_Parse(&p), "0:04");
}

Test(q_shared, com_parse_keeps_braces_attached_to_tokens) {
	/* COM_Parse does not treat { or } as separators, which is why the music
	   playlist parser has to inspect the first and last characters of a token
	   to detect block delimiters. */
	char  text[] = "battle{ }";
	char *p = text;

	cr_assert_str_eq(COM_Parse(&p), "battle{");
	cr_assert_str_eq(COM_Parse(&p), "}");
}

Test(q_shared, com_parse_skips_line_comments) {
	char  text[] = "one // trailing comment\ntwo";
	char *p = text;

	cr_assert_str_eq(COM_Parse(&p), "one");
	cr_assert_str_eq(COM_Parse(&p), "two");
}

Test(q_shared, com_parse_handles_quoted_strings) {
	char  text[] = "\"two words\" next";
	char *p = text;

	cr_assert_str_eq(COM_Parse(&p), "two words");
	cr_assert_str_eq(COM_Parse(&p), "next");
}

/* ---------------------------------------------------------------------- va */

Test(q_shared, va_formats) {
	cr_assert_str_eq(va("%i-%s", 9001, "over"), "9001-over");
}
