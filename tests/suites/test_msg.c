/*
Shared/msg.c - bit-level network serialisation.

Two jobs here.

1. Characterisation. msg.c IS the wire protocol; if a read and a write ever
   disagree, clients and servers desync in ways that surface far from the cause.
   Before touching it for the UBSan findings, these round-trip tests pin the
   observable behaviour down so a "cleanup" that changes a value cannot pass
   unnoticed.

2. A red test for a real defect. MSG_WriteBits' signed-overflow check computes
   `1 << (bits-1)` with a negative `bits`, which is undefined behaviour (a
   negative shift exponent) - see msg_writebits_signed_is_not_undefined below.
   Under UBSan that test fails today and is the reason the fix is safe to make.

Bit-width convention: a positive `bits` means an unsigned field of that width,
a negative `bits` means a signed field of |bits| bits. MSG_WriteBits rejects
0, anything below -31 and anything above 32.
*/

#include <criterion/criterion.h>

#include "q_shared.h"
#include "qcommon.h"
#include "bg_public.h"

static byte  buffer[8192];
static msg_t msg;

static void msg_setup(void) {
	memset(buffer, 0, sizeof(buffer));
	MSG_Init(&msg, buffer, sizeof(buffer));
	/* Huffman tables are lazily built by the first Init; nothing else needed. */
}

TestSuite(msg, .init = msg_setup);

/* --------------------------------------------------------- round-tripping */

Test(msg, unsigned_widths_round_trip) {
	const int widths[] = { 1, 2, 3, 7, 8, 9, 15, 16, 17, 23, 24, 31, 32 };
	size_t i;

	for (i = 0; i < ARRAY_LEN(widths); ++i) {
		int bits = widths[i];
		/* Largest value representable in this many bits. */
		unsigned int value = (bits == 32) ? 0xFFFFFFFFu
		                                  : (unsigned int)((1u << bits) - 1u);
		msg_setup();
		MSG_WriteBits(&msg, (int)value, bits);
		MSG_BeginReading(&msg);
		cr_assert_eq((unsigned int)MSG_ReadBits(&msg, bits), value,
		             "width %d did not round-trip", bits);
	}
}

Test(msg, zero_round_trips_at_every_width) {
	int bits;

	for (bits = 1; bits <= 32; ++bits) {
		msg_setup();
		MSG_WriteBits(&msg, 0, bits);
		MSG_BeginReading(&msg);
		cr_assert_eq(MSG_ReadBits(&msg, bits), 0, "width %d", bits);
	}
}

Test(msg, signed_widths_round_trip) {
	const int widths[] = { -2, -4, -8, -16, -31 };
	size_t i;

	for (i = 0; i < ARRAY_LEN(widths); ++i) {
		int bits = widths[i];
		int magnitude = -bits;
		int values[3];
		size_t v;

		values[0] = 0;
		values[1] = (1 << (magnitude - 1)) - 1;   /* most positive */
		values[2] = -(1 << (magnitude - 1));      /* most negative */

		for (v = 0; v < ARRAY_LEN(values); ++v) {
			msg_setup();
			MSG_WriteBits(&msg, values[v], bits);
			MSG_BeginReading(&msg);
			cr_assert_eq(MSG_ReadBits(&msg, bits), values[v],
			             "signed width %d value %d did not round-trip",
			             bits, values[v]);
		}
	}
}

Test(msg, sequential_fields_keep_their_boundaries) {
	msg_setup();
	MSG_WriteBits(&msg, 1, 1);
	MSG_WriteBits(&msg, 0x2A, 7);
	MSG_WriteBits(&msg, 0x1234, 16);
	MSG_WriteBits(&msg, -5, -8);

	MSG_BeginReading(&msg);
	cr_assert_eq(MSG_ReadBits(&msg, 1), 1);
	cr_assert_eq(MSG_ReadBits(&msg, 7), 0x2A);
	cr_assert_eq(MSG_ReadBits(&msg, 16), 0x1234);
	cr_assert_eq(MSG_ReadBits(&msg, -8), -5);
}

Test(msg, byte_values_with_the_high_bit_set_round_trip) {
	/* MSG_ReadBits assembles bytes with `value |= get << shift`. With a byte
	   >= 0x80 and a shift of 24 that overflows a signed int, so these values
	   are the ones a careless fix would corrupt. */
	const unsigned int values[] = { 0x80u, 0xFFu, 0x80000000u, 0xFF000000u, 0xDEADBEEFu };
	size_t i;

	for (i = 0; i < ARRAY_LEN(values); ++i) {
		msg_setup();
		MSG_WriteBits(&msg, (int)values[i], 32);
		MSG_BeginReading(&msg);
		cr_assert_eq((unsigned int)MSG_ReadBits(&msg, 32), values[i],
		             "0x%08x did not round-trip", values[i]);
	}
}

Test(msg, long_and_string_round_trip) {
	msg_setup();
	MSG_WriteLong(&msg, -123456789);
	MSG_WriteString(&msg, "over nine thousand");

	MSG_BeginReading(&msg);
	cr_assert_eq(MSG_ReadLong(&msg), -123456789);
	cr_assert_str_eq(MSG_ReadString(&msg), "over nine thousand");
}

/* --------------------------------------------------- red test for the bug */

/*
MSG_WriteBits' overflow check, for a signed field, does:

    int r = 1 << (bits-1);

`bits` is negative for signed fields, so this shifts by a negative exponent -
undefined behaviour, and the check it feeds is meaningless. UBSan reports
"shift exponent -17 is negative" from msg.c:132.

The intent is plainly `1 << (|bits| - 1)`. This test drives every signed width
the API accepts; with -fsanitize=undefined it fails until that is corrected.

It asserts on the round-trip rather than on the (internal, diagnostic-only)
overflow counter, so it stays meaningful after the fix instead of pinning an
implementation detail.
*/
Test(msg, msg_writebits_signed_is_not_undefined) {
	int bits;

	for (bits = -1; bits >= -31; --bits) {
		msg_setup();
		MSG_WriteBits(&msg, -1, bits);
		MSG_BeginReading(&msg);
		cr_assert_eq(MSG_ReadBits(&msg, bits), -1,
		             "signed width %d", bits);
	}
}

/*
The unsigned side of the same check does `1 << bits` with bits up to 31, which
overflows a signed int at 31. Same class of defect, same fix shape.
*/
Test(msg, msg_writebits_unsigned_31_is_not_undefined) {
	msg_setup();
	MSG_WriteBits(&msg, 0x7FFFFFFF, 31);
	MSG_BeginReading(&msg);
	cr_assert_eq((unsigned int)MSG_ReadBits(&msg, 31), 0x7FFFFFFFu);
}

/* ----------------------------------------------- delta-encoded wide fields */

/*
powerLevel[] rides the playerstate delta as full 32-bit values. It used to go
over the wire as shorts, so anything past 32767 - a big damage accumulation, a
raised plLimit - arrived truncated or sign-flipped, and a negative accumulator
read as a heal on the client.
*/
Test(msg, playerstate_powerlevel_survives_values_beyond_16_bits) {
	playerState_t to, out;
	int i;

	memset(&to, 0, sizeof(to));
	memset(&out, 0, sizeof(out));
	for (i = 0; i < MAX_POWERSTATS; ++i) {
		to.powerLevel[i] = 32768 + i * 1000003;   /* all beyond the old short range */
	}
	to.powerLevel[0] = 100000000;    /* POWERLEVEL_MAX */
	to.powerLevel[1] = 8000000;      /* the hurt_touch kill credit */
	to.powerLevel[2] = -100000000;   /* plCurrent may legitimately go negative */
	to.powerLevel[3] = 9001;         /* and small values still round-trip */

	msg_setup();
	MSG_WriteDeltaPlayerstate(&msg, NULL, &to);
	MSG_BeginReading(&msg);
	MSG_ReadDeltaPlayerstate(&msg, NULL, &out);

	for (i = 0; i < MAX_POWERSTATS; ++i) {
		cr_assert_eq(out.powerLevel[i], to.powerLevel[i],
		             "powerLevel[%d]: wrote %d, read %d",
		             i, to.powerLevel[i], out.powerLevel[i]);
	}
}

/*
attackPowerTotal/Current mirror plMaximum/plHealth into the entity state for
other clients' health bars; their netfield widths have to keep up with the
powerLevel range or spectators see wrapped values the owner does not.
*/
Test(msg, entitystate_attackpower_survives_values_beyond_16_bits) {
	entityState_t from, to, out;
	int number;

	memset(&from, 0, sizeof(from));
	memset(&to, 0, sizeof(to));
	memset(&out, 0, sizeof(out));
	to.number = 5;
	to.attackPowerTotal = 100000000;
	to.attackPowerCurrent = 99999999;

	msg_setup();
	MSG_WriteDeltaEntity(&msg, &from, &to, qtrue);
	MSG_BeginReading(&msg);
	number = MSG_ReadBits(&msg, GENTITYNUM_BITS);
	MSG_ReadDeltaEntity(&msg, &from, &out, number);

	cr_assert_eq(out.number, 5);
	cr_assert_eq(out.attackPowerTotal, 100000000);
	cr_assert_eq(out.attackPowerCurrent, 99999999);
}

/* ------------------------------------------------- playerState delta fields */

/*
The limit break accumulator lives in playerState_t's buffers[] rather than in a
function static, and that only works if it reaches the client: prediction
re-runs pmove from the last snapshot every frame, so an accumulator the snapshot
does not carry restarts from whatever the delta baseline happened to hold.

This also pins the reason the move needed no protocol change. buffers[] is
delta-coded behind a mask a fixed MAX_RBUFFERS bits wide, so a second entry in
the enum costs nothing on the wire - but only while the entry is inside
MAX_RBUFFERS, which is what the bounds assertion below is for.
*/
Test(msg, buffers_survive_a_playerstate_delta) {
	playerState_t from, to, out;

	cr_assert_lt(bfBreakLimit, MAX_RBUFFERS,
		"the limit break pool must be inside the mask buffers[] is coded behind");

	memset(&from, 0, sizeof(from));
	memset(&to, 0, sizeof(to));
	memset(&out, 0, sizeof(out));
	to.buffers[bfBreakLimit] = 0.375f;
	to.buffers[bfZanzokenCost] = 0.5f;

	MSG_WriteDeltaPlayerstate(&msg, &from, &to);
	MSG_BeginReading(&msg);
	MSG_ReadDeltaPlayerstate(&msg, &from, &out);

	cr_assert_float_eq(out.buffers[bfBreakLimit], 0.375f, 1e-6,
		"the limit break pool did not survive the wire");
	cr_assert_float_eq(out.buffers[bfZanzokenCost], 0.5f, 1e-6,
		"the neighbouring buffer entry was disturbed");
}
