/*
Engine/renderer/tr_image_png.c - PNG decoding.

The loader walks the file by casting pointers into the raw file buffer to
struct types and reading uint32_t fields directly:

    CH = BufferedFileRead(BF, PNG_ChunkHeader_Size);
    Length = BigLong(CH->Length);

A PNG's chunk headers do not sit at 4-byte boundaries - the 8-byte signature is
followed by a 13-byte IHDR payload and a 4-byte CRC, so every later chunk header
lands at an odd offset. Reading a uint32_t through such a pointer is undefined
behaviour. On arm64 macOS it happens to work; on a strict-alignment target it
faults, and it is one of the few UBSan findings that is a real portability
hazard rather than a theoretical one.

These tests assert the observable contract - the decode must produce the right
pixels - so they stay meaningful after the reads are made alignment-safe. Under
-fsanitize=undefined they also fail while the misaligned loads remain, which is
what makes them the red test for that fix.

The fixture is embedded (see fixtures/png_2x2_rgba.h) so this suite needs no
game assets.
*/

#include <criterion/criterion.h>

#include "tr_local.h"

#include "png_fixtures.h"

/* -------------------------------------------------------- the ri seam */

refimport_t ri;

static const unsigned char *fake_file_data;
static int                  fake_file_len;

static long fake_FS_ReadFile(const char *name, void **buf) {
	(void)name;
	if (!fake_file_data) {
		if (buf) {
			*buf = NULL;
		}
		return -1;
	}
	if (!buf) {
		return fake_file_len;
	}
	*buf = malloc((size_t)fake_file_len);
	memcpy(*buf, fake_file_data, (size_t)fake_file_len);
	return fake_file_len;
}

static void fake_FS_FreeFile(void *buf) {
	free(buf);
}

static void *fake_Malloc(int bytes) {
	return malloc((size_t)bytes);
}

static void fake_Free(void *buf) {
	free(buf);
}

static void QDECL fake_Printf(int level, const char *fmt, ...) {
	va_list ap;
	(void)level;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void png_setup(void) {
	memset(&ri, 0, sizeof(ri));
	ri.FS_ReadFile = fake_FS_ReadFile;
	ri.FS_FreeFile = fake_FS_FreeFile;
	ri.Malloc      = fake_Malloc;
	ri.Free        = fake_Free;
	ri.Printf      = fake_Printf;

	fake_file_data = png_2x2_rgba;
	fake_file_len  = (int)sizeof(png_2x2_rgba);
}

TestSuite(png, .init = png_setup);

/* ------------------------------------------------------------- the tests */

Test(png, decodes_dimensions) {
	byte *pic = NULL;
	int   w = 0, h = 0;

	R_LoadPNG("test.png", &pic, &w, &h);

	cr_assert_not_null(pic, "loader returned no image");
	cr_assert_eq(w, PNG_2X2_WIDTH);
	cr_assert_eq(h, PNG_2X2_HEIGHT);
	free(pic);
}

Test(png, decodes_pixels_exactly) {
	byte *pic = NULL;
	int   w = 0, h = 0;

	R_LoadPNG("test.png", &pic, &w, &h);
	cr_assert_not_null(pic);

	/* RGBA, row-major, straight from the fixture generator. */
	cr_assert_arr_eq(pic, png_2x2_expected, sizeof(png_2x2_expected));
	free(pic);
}

/*
The chunk header immediately after IHDR sits at byte 33, which is not a multiple
of 4. That is the offset the loader dereferences as a uint32_t, so a correct
decode of this fixture is what proves the reads were made alignment-safe.
*/
Test(png, chunk_headers_are_not_four_byte_aligned) {
	/* signature 8 + (len 4 + type 4 + IHDR payload 13 + crc 4) = 33 */
	cr_assert_neq(33 % 4, 0,
	              "fixture no longer exercises unaligned chunk headers");
}

Test(png, missing_file_is_handled) {
	byte *pic = NULL;
	int   w = -1, h = -1;

	fake_file_data = NULL;
	R_LoadPNG("nope.png", &pic, &w, &h);

	cr_assert_null(pic, "a missing file must not yield an image");
}

Test(png, truncated_file_is_rejected_without_crashing) {
	byte *pic = NULL;
	int   w = 0, h = 0;

	fake_file_len = 20;   /* signature + a partial IHDR */
	R_LoadPNG("truncated.png", &pic, &w, &h);

	cr_assert_null(pic, "a truncated PNG must not yield an image");
}

Test(png, garbage_signature_is_rejected) {
	static unsigned char junk[64];
	byte *pic = NULL;
	int   w = 0, h = 0;

	memset(junk, 0xAB, sizeof(junk));
	fake_file_data = junk;
	fake_file_len  = (int)sizeof(junk);

	R_LoadPNG("junk.png", &pic, &w, &h);
	cr_assert_null(pic);
}

/* ------------------------------------------------- other decoder paths */

/*
One truecolour-alpha image leaves most of the decoder untested. The loader
branches hard on ColourType and bit depth, and each branch does its own pointer
walking over the file buffer, so these exist mainly to put the grey, truecolour,
indexed and 4-bit-packed paths under ASan/UBSan. Exact pixel expectations are
deliberately not asserted for the palette paths - the alpha compositing rules are
involved enough that a wrong expectation here would fail for the wrong reason.
*/

static void decodes_ok(const unsigned char *data, int len, int w_exp, int h_exp,
                       const char *what) {
	byte *pic = NULL;
	int   w = 0, h = 0;

	fake_file_data = data;
	fake_file_len  = len;

	R_LoadPNG("fixture.png", &pic, &w, &h);

	cr_assert_not_null(pic, "%s failed to decode", what);
	cr_assert_eq(w, w_exp, "%s width", what);
	cr_assert_eq(h, h_exp, "%s height", what);
	free(pic);
}

Test(png, decodes_greyscale_with_trns) {
	decodes_ok(png_grey_trns, (int)sizeof(png_grey_trns), 2, 2, "greyscale+tRNS");
}

Test(png, decodes_truecolour_with_trns) {
	decodes_ok(png_true_trns, (int)sizeof(png_true_trns), 2, 2, "truecolour+tRNS");
}

Test(png, decodes_indexed8_with_palette_and_trns) {
	decodes_ok(png_indexed8_plte_trns, (int)sizeof(png_indexed8_plte_trns),
	           3, 2, "indexed8+PLTE+tRNS");
}

Test(png, decodes_indexed4_packed_two_pixels_per_byte) {
	decodes_ok(png_indexed4_plte, (int)sizeof(png_indexed4_plte),
	           4, 2, "indexed4+PLTE");
}

Test(png, greyscale_alpha_comes_from_trns) {
	/* Grey 0x40 is declared transparent by the fixture's tRNS chunk, and it is
	   the second pixel of the first row. Everything else stays opaque. */
	byte *pic = NULL;
	int   w = 0, h = 0;

	fake_file_data = png_grey_trns;
	fake_file_len  = (int)sizeof(png_grey_trns);
	R_LoadPNG("grey.png", &pic, &w, &h);
	cr_assert_not_null(pic);

	cr_assert_eq(pic[0 * 4 + 3], 255, "grey 0x00 should be opaque");
	cr_assert_eq(pic[1 * 4 + 3], 0,   "grey 0x40 is the tRNS colour");
	/* Greyscale expands to R == G == B. */
	cr_assert_eq(pic[1 * 4 + 0], pic[1 * 4 + 1]);
	cr_assert_eq(pic[1 * 4 + 1], pic[1 * 4 + 2]);
	free(pic);
}

/* --------------------------------- malformed tRNS: the red tests */

/*
tr_image_png.c validated the tRNS chunk length with

    if(!ChunkHeaderLength == 2)      /-* and == 6 for truecolour *-/

which parses as `(!ChunkHeaderLength) == 2`. That is 0 or 1 and can never equal
2, so the condition is always false and the check rejects nothing.

The consequence is not cosmetic: Trans points at exactly ChunkHeaderLength bytes,
but the truecolour branch then reads Trans[0] through Trans[5] unconditionally.
A 2-byte tRNS is therefore read as 6 - an out-of-bounds read driven purely by
file content, and PNGs in this game arrive from downloaded pk3 files and custom
player skins.

Both of these fail while the checks are dead, because a malformed file decodes
successfully instead of being refused.
*/

Test(png, truecolour_trns_of_wrong_length_is_rejected) {
	byte *pic = NULL;
	int   w = 0, h = 0;

	fake_file_data = png_bad_trns_true;
	fake_file_len  = (int)sizeof(png_bad_trns_true);

	R_LoadPNG("bad_true.png", &pic, &w, &h);

	cr_assert_null(pic,
	               "a truecolour tRNS of 2 bytes must be refused; the branch "
	               "reads 6 bytes from it");
	free(pic);
}

Test(png, greyscale_trns_of_wrong_length_is_rejected) {
	byte *pic = NULL;
	int   w = 0, h = 0;

	fake_file_data = png_bad_trns_grey;
	fake_file_len  = (int)sizeof(png_bad_trns_grey);

	R_LoadPNG("bad_grey.png", &pic, &w, &h);

	cr_assert_null(pic, "a greyscale tRNS of 4 bytes must be refused");
	free(pic);
}
