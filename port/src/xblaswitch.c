/**
 * The release's assets on one switch. See xblaswitch.h for what this is;
 * this file is the how, and it is five calls and a key.
 *
 * Nothing here knows anything about the release - each part is asked to switch
 * itself, and every one of them was already a live toggle for the sake of its
 * own checkbox. What this adds is that they move together.
 */

#include <string.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "types.h"
#include "config.h"
#include "system.h"
#include "input.h"
#include "xblamesh.h"
#include "xblatex.h"
#include "xblastage.h"
#include "xblafont.h"
#include "xblaexpl.h"
#include "xblaswitch.h"

#ifndef PLATFORM_N64

s32 xblaSwitchGetEnabled(void)
{
	return xblaMeshGetEnabled()
		&& xblaTexGetEnabled()
		&& xblaStageGetEnabled()
		&& xblaFontGetEnabled()
		&& xblaExplGetEnabled();
}

void xblaSwitchSetEnabled(s32 enabled)
{
	enabled = enabled ? 1 : 0;

	// The rooms before the models: both setters drop the rooms loaded under
	// the old setting (xblaStageSwitched()), and the rooms count only while
	// the meshes are on, so setting them this way round means the models have
	// the last word and the drop that matters is the last one.
	xblaStageSetEnabled(enabled);
	xblaMeshSetEnabled(enabled);

	xblaTexSetEnabled(enabled);
	xblaFontSetEnabled(enabled);
	xblaExplSetEnabled(enabled);
}

/**
 * Resolved to a scancode on first use, as texpack.c does: inputInit() fills
 * the table the name is looked up in, so it cannot be done when the config is
 * read.
 */
#define XBLASWITCH_KEYNAME_LEN 32
static char toggleKeyName[XBLASWITCH_KEYNAME_LEN] = "F6";
static s32 toggleKeyVk = -1;

s32 xblaSwitchGetKey(void)
{
	if (toggleKeyVk < 0) {
		if (!toggleKeyName[0] || !strcmp(toggleKeyName, "NONE")) {
			toggleKeyVk = 0;
		} else {
			toggleKeyVk = inputGetKeyByName(toggleKeyName);

			if (toggleKeyVk < 0) {
				toggleKeyVk = 0;
			}
		}
	}

	return toggleKeyVk;
}

void xblaSwitchSetKey(s32 vk)
{
	if (vk <= 0 || vk >= VK_TOTAL_COUNT) {
		toggleKeyName[0] = '\0';
		toggleKeyVk = 0;
		return;
	}

	strncpy(toggleKeyName, inputGetKeyName(vk), sizeof(toggleKeyName) - 1);
	toggleKeyName[sizeof(toggleKeyName) - 1] = '\0';
	toggleKeyVk = vk;
}

void xblaSwitchTick(void)
{
	const s32 vk = xblaSwitchGetKey();

	// inputKeyJustPressed() consumes the edge, so ask once a frame and only
	// when the key is actually bound.
	if (vk > 0 && inputKeyJustPressed(vk)) {
		const s32 enabled = !xblaSwitchGetEnabled();

		xblaSwitchSetEnabled(enabled);

		sysLogPrintf(LOG_NOTE, "xblaswitch: release assets %s%s", enabled ? "on" : "off",
				enabled && !xblaMeshIsAvailable() ? " (no package found)" : "");
	}
}

PD_CONSTRUCTOR static void xblaSwitchConfigInit(void)
{
	// Still Mod.XblaMeshKey, which is what it was when the key was the meshes
	// alone: it is the same key doing more, and renaming it would silently put
	// everyone who has bound their own back on F6.
	configRegisterString("Mod.XblaMeshKey", toggleKeyName, sizeof(toggleKeyName));
}

#else

s32 xblaSwitchGetEnabled(void) { return 0; }
void xblaSwitchSetEnabled(s32 enabled) { }
s32 xblaSwitchGetKey(void) { return 0; }
void xblaSwitchSetKey(s32 vk) { }
void xblaSwitchTick(void) { }

#endif
