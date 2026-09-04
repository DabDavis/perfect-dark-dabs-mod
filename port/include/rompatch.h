#ifndef _IN_ROMPATCH_H
#define _IN_ROMPATCH_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

enum rompatchkind {
	ROMPATCH_NONE = 0,
	ROMPATCH_XDELTA,
	ROMPATCH_BPS,
	ROMPATCH_IPS,
};

/**
 * What kind of patch these opening bytes announce, or ROMPATCH_NONE.
 */
s32 rompatchIdentify(const u8 *head, u32 len);

/**
 * Whether the file name has a patch extension (.xdelta, .vcdiff, .bps, .ips).
 */
s32 rompatchIsPatchName(const char *name);

const char *rompatchKindName(s32 kind);

/**
 * Applies a patch to a ROM. On success returns the kind and hands back a
 * malloc'd patched ROM in *out; on failure returns -1 with the reason in err.
 *
 * xdelta patches are decoded as plain VCDIFF (RFC 3284) with xdelta3's
 * application header and per-window checksum. Secondary compression and
 * custom code tables are refused with a message, not silently misread.
 */
s32 rompatchApply(const u8 *rom, u32 romlen, const u8 *patch, u32 patchlen,
		u8 **out, u32 *outlen, char *err, u32 errlen);

#ifdef __cplusplus
}
#endif

#endif
