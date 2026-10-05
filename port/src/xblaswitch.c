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
#include "net/net.h"
#include "input.h"
#include "xblamesh.h"
#include "xblatex.h"
#include "xblastage.h"
#include "xblafont.h"
#include "xblaexpl.h"
#include "xblasky.h"
#include "xblaswitch.h"
#include "gexfront.h"
#include "xblatables.h"
#include "game/title.h"
#include "game/hudmsg.h"
#include "langpack.h"
#include "trace.h"
#include "modloader.h"
#include "lib/main.h"

#ifndef PLATFORM_N64

/**
 * A GoldenEye ROM hack's arenas (Goldfinger 64's: a remake stage that is not
 * GE Plus's own) are the cartridge's look only. The release has nothing of a
 * hack's own art, and its GoldenEye meshes go by GoldenEye's file names, which
 * a hack gives its own props - so every part reads as off while one is the
 * stage (user, 2026-10-02: "disable xbla mode for Goldfinger 64, since it would
 * have incomplete textures"). The parts' own switches, and the player's pd.ini,
 * are left as they are; the next stage has them back.
 */
s32 xblaSwitchStageHeld(void)
{
	static s32 stage = -1;
	static s32 held;
	const s32 now = mainGetStageNum();

	if (now != stage) {
		stage = now;
		held = modloaderStageIsRemake(now) && !modloaderStageIsGexPlus(now);
	}

	return held;
}

s32 xblaSwitchGetEnabled(void)
{
	return xblaMeshGetEnabled()
		&& xblaTexGetEnabled()
		&& xblaStageGetEnabled()
		&& xblaFontGetEnabled()
		&& xblaExplGetEnabled()
		&& xblaSkyGetEnabled()
		&& xblaMeshGetReflections();
}

u32 xblaSwitchGetParts(void)
{
	return (xblaMeshGetEnabled() ? XBLASWITCH_PART_MESHES : 0)
		| (xblaTexGetEnabled() ? XBLASWITCH_PART_TEXTURES : 0)
		| (xblaStageGetEnabled() ? XBLASWITCH_PART_STAGES : 0)
		| (xblaFontGetEnabled() ? XBLASWITCH_PART_FONT : 0)
		| (xblaExplGetEnabled() ? XBLASWITCH_PART_EXPLOSIONS : 0)
		| (xblaSkyGetEnabled() ? XBLASWITCH_PART_SKIES : 0)
		| (xblaMeshGetReflections() ? XBLASWITCH_PART_REFLECTIONS : 0);
}

void xblaSwitchSetParts(u32 parts)
{
	// netplay: a match's rules stay as they started (H14)
	if (g_NetMode != NETMODE_NONE && netRulesLocked()) return;

	// The rooms before the models: both setters drop the rooms loaded under
	// the old setting (xblaStageSwitched()), and the rooms count only while
	// the meshes are on, so setting them this way round means the models have
	// the last word and the drop that matters is the last one.
	xblaStageSetEnabled((parts & XBLASWITCH_PART_STAGES) != 0);
	xblaMeshSetEnabled((parts & XBLASWITCH_PART_MESHES) != 0);

	xblaTexSetEnabled((parts & XBLASWITCH_PART_TEXTURES) != 0);

	// GE Plus's folder screens wear the release's art with the meshes on
	// (gefolder.c), and the switch is live while the folder is up
	gexFrontMeshesSwitched();
	xblaFontSetEnabled((parts & XBLASWITCH_PART_FONT) != 0);
	xblaExplSetEnabled((parts & XBLASWITCH_PART_EXPLOSIONS) != 0);
	xblaSkySetEnabled((parts & XBLASWITCH_PART_SKIES) != 0);
	xblaMeshSetReflections((parts & XBLASWITCH_PART_REFLECTIONS) != 0);
}

void xblaSwitchSetEnabled(s32 enabled)
{
	xblaSwitchSetParts(enabled ? XBLASWITCH_PART_ALL : 0);
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

	// Every frame, not on the key: the tables follow the switch however it was
	// moved (a checkbox, the settings preset), and a mod load copies its own
	// snapshot of them back over whatever they held.
	xblaTablesTick();

	// inputKeyJustPressed() consumes the edge, so ask once a frame and only
	// when the key is actually bound.
	if (vk > 0 && inputKeyJustPressed(vk)) {
		if (xblaSwitchStageHeld()) {
			sysLogPrintf(LOG_NOTE, "xblaswitch: ignored on a ROM hack's arena, which is the cartridge's look only");
			hudmsgSayToggle(langTr("XBLA Assets are off on this map\n"), langTr("XBLA Assets are off on this map\n"), 0);
			return;
		}

		// Not while the boot logos play: with the release on they are 4J's
		// meshes, decided as each logo starts, and a model switched out from
		// under an intro that has already chosen how to draw it is not worth
		// the risk for a sequence that is over in fifteen seconds.
		if (titleIsBootSequence()) {
			sysLogPrintf(LOG_NOTE, "xblaswitch: ignored during the boot logos");
			return;
		}

		const s32 enabled = !xblaSwitchGetEnabled();

		xblaSwitchSetEnabled(enabled);

		traceNoteEvent("release assets key: %s", enabled ? "on" : "off");
		sysLogPrintf(LOG_NOTE, "xblaswitch: release assets %s%s", enabled ? "on" : "off",
				enabled && !xblaMeshIsAvailable() ? " (no package found)" : "");

		// Said on screen as the other toggle keys are (F3 20260930-021639-f11aab1d)
		hudmsgSayToggle(langTr("XBLA Assets On\n"), langTr("XBLA Assets Off\n"), enabled);
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

s32 xblaSwitchStageHeld(void) { return 0; }
s32 xblaSwitchGetEnabled(void) { return 0; }
void xblaSwitchSetEnabled(s32 enabled) { }
u32 xblaSwitchGetParts(void) { return 0; }
void xblaSwitchSetParts(u32 parts) { }
s32 xblaSwitchGetKey(void) { return 0; }
void xblaSwitchSetKey(s32 vk) { }
void xblaSwitchTick(void) { }

#endif
