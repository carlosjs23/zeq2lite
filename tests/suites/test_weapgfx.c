/*
Game/CGame/cg_userweapons.h - the weapon-graphics parse buffer's layout.

cg_weapGfxParser.c used to size a copy into weaponName[MAX_WEAPONNAME] (40
bytes) with sizeof(missileTrailSpiralShader) (MAX_QPATH, 64). Because
Q_strncpyz wraps strncpy - which pads to n bytes - that wrote 63 bytes plus a
terminator on every call regardless of input length, running 24 bytes past the
end of the object. It ran during CG_RegisterClients, so it corrupted memory
every time a client loaded.

What this suite can and cannot see: it verifies the struct layout and the copy
semantics, not which bound the parser's source actually passes. Two other checks
cover that:

  tests/lint/check_strncpyz_field_sizes.py - reads the call sites
  Tools/dev/zeq2sanitize.sh                - runs the real parser under ASan

Together they cover the defect at three levels; alone, none of them would.
*/

#include <criterion/criterion.h>

#include "q_shared.h"
#include "cg_userweapons.h"

/* Global, so an overrun is reported as a global-buffer-overflow exactly as it
   was in the running game. */
static cg_userWeaponParseBuffer_t buf;

Test(weapgfx, weaponname_is_the_last_member) {
	size_t offset = (size_t)((char *)buf.weaponName - (char *)&buf);

	/* If it stops being last, an overrun corrupts a sibling field instead of
	   running off the object - quieter, and harder to find. */
	cr_assert_eq(offset + sizeof(buf.weaponName), sizeof(buf),
	             "weaponName is no longer the final member; revisit the bound checks");
}

Test(weapgfx, weaponname_is_smaller_than_the_field_that_used_to_size_it) {
	/* Documents why the historical bound was wrong. If these ever match, the
	   old bug becomes unreachable and this suite is moot - worth failing loudly
	   rather than passing silently on a stale premise. */
	cr_assert_lt(sizeof(buf.weaponName), sizeof(buf.missileTrailSpiralShader));
}

Test(weapgfx, copying_a_long_name_stays_in_bounds) {
	char longname[256];

	memset(longname, 'A', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = '\0';

	Q_strncpyz(buf.weaponName, longname, sizeof(buf.weaponName));

	cr_assert_eq(strlen(buf.weaponName), sizeof(buf.weaponName) - 1);
	cr_assert_eq(buf.weaponName[sizeof(buf.weaponName) - 1], '\0');
}

Test(weapgfx, copying_an_empty_name_stays_in_bounds) {
	/* The sibling call site copied "" and was just as unsafe, because the
	   padding is unconditional. */
	Q_strncpyz(buf.weaponName, "", sizeof(buf.weaponName));
	cr_assert_eq(buf.weaponName[0], '\0');
}

Test(weapgfx, weaponicon_bound_matches_its_own_field) {
	char longpath[256];

	memset(longpath, 'B', sizeof(longpath) - 1);
	longpath[sizeof(longpath) - 1] = '\0';

	Q_strncpyz(buf.weaponIcon, longpath, sizeof(buf.weaponIcon));
	cr_assert_eq(strlen(buf.weaponIcon), sizeof(buf.weaponIcon) - 1);
}
