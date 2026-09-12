#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "data.h"
#include "types.h"
#include "game/mainmenu.h"
#include "game/menu.h"
#include "game/menugfx.h"
#include "game/game_1531a0.h"
#include "lib/vi.h"
#include "bss.h"
#include "game/gamefile.h"
#include "game/player.h"
#include "game/modoptions.h"
#include "game/modrandom.h"
#include "game/modghost.h"
#include "game/modspectate.h"
#include "lib/joy.h"
#include "video.h"
#include "input.h"
#include "config.h"
#include "record.h"
#include "screenshot.h"
#include "mod.h"
#include "modloader.h"
#include "system.h"
#include "texpack.h"
#include "xblaimport.h"
#include "xblamesh.h"
#include "modelpack.h"
#include "assetdump.h"
#include "xblatex.h"
#include "xblafont.h"
#include "xblaui.h"
#include "xblaexpl.h"
#include "menuimage.h"
#include "xblastage.h"

static s32 g_ExtMenuPlayer = 0;
static struct menudialogdef *g_ExtNextDialog = NULL;

static s32 g_BindIndex = 0;
static u32 g_BindContKey = 0;

// The bind dialog is shared. The screenshot and recording keys are not
// controller buttons, so a row that owns one hands the dialog the setter to call
// with the key that gets pressed; a controller row leaves it null and the key
// goes to inputKeyBind() as before.
static void (*g_BindKeySetter)(s32 vk) = NULL;

static MenuItemHandlerResult menuhandlerSelectPlayer(s32 operation, struct menuitem *item, union handlerdata *data);

struct menuitem g_ExtendedSelectPlayerMenuItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Player 1\n",
		0,
		menuhandlerSelectPlayer,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Player 2\n",
		0,
		menuhandlerSelectPlayer,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Player 3\n",
		0,
		menuhandlerSelectPlayer,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Player 4\n",
		0,
		menuhandlerSelectPlayer,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedSelectPlayerMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Select Player",
	g_ExtendedSelectPlayerMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerSelectPlayer(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_ExtMenuPlayer = item - g_ExtendedSelectPlayerMenuItems;
		((char *)g_ExtNextDialog->title)[7] = g_ExtMenuPlayer + '1';
		menuPushDialog(g_ExtNextDialog);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMouseEnabled(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return inputMouseIsEnabled();
	case MENUOP_SET:
		inputMouseEnable(data->checkbox.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMouseAimLock(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimmode;
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimmode = data->checkbox.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMouseLockMode(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = {
		"Always Off",
		"Always On",
		"Auto"
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		inputSetMouseLockMode(data->checkbox.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = inputGetMouseLockMode();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMenuMouseControl(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_MenuMouseControl;
	case MENUOP_SET:
		g_MenuMouseControl = data->checkbox.value;
		if (!g_MenuMouseControl) {
			g_MenuUsingMouse = false;
		}
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMouseSpeedX(s32 operation, struct menuitem *item, union handlerdata *data)
{
	f32 x, y;

	switch (operation) {
	case MENUOP_GETSLIDER:
		inputMouseGetSpeed(&x, &y);
		if (x < 0.f) {
			data->slider.value = 0;
		} else {
			data->slider.value = x * 100.f + 0.5f;
		}
		break;
	case MENUOP_SET:
		inputMouseGetSpeed(&x, &y);
		inputMouseSetSpeed((f32)data->slider.value / 100.f, y);
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%.2f", (f32)data->slider.value / 100.f);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMouseSpeedY(s32 operation, struct menuitem *item, union handlerdata *data)
{
	f32 x, y;

	switch (operation) {
	case MENUOP_GETSLIDER:
		inputMouseGetSpeed(&x, &y);
		if (y < 0.f) {
			data->slider.value = 0;
		} else {
			data->slider.value = y * 100.f + 0.5f;
		}
		break;
	case MENUOP_SET:
		inputMouseGetSpeed(&x, &y);
		inputMouseSetSpeed(x, (f32)data->slider.value / 100.f);
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%.2f", (f32)data->slider.value / 100.f);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMouseAimSpeedX(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		if (g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimspeedx < 0.f) {
			data->slider.value = 0;
		} else if (g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimspeedx > 10.f) {
			data->slider.value = 1000;
		} else {
			data->slider.value = g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimspeedx * 100.f + 0.5f;
		}
		break;
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimspeedx = (f32)data->slider.value / 100.f;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%.2f", (f32)data->slider.value / 100.f);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMouseAimSpeedY(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		if (g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimspeedy < 0.f) {
			data->slider.value = 0;
		} else if (g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimspeedy > 10.f) {
			data->slider.value = 1000;
		} else {
			data->slider.value = g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimspeedy * 100.f + 0.5f;
		}
		break;
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].mouseaimspeedy = (f32)data->slider.value / 100.f;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%.2f", (f32)data->slider.value / 100.f);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerRadialMenuSpeed(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		if (g_PlayerExtCfg[0].radialmenuspeed < 0.f) {
			data->slider.value = 0;
		} else if (g_PlayerExtCfg[0].radialmenuspeed > 10.f) {
			data->slider.value = 1000;
		} else {
			data->slider.value = g_PlayerExtCfg[0].radialmenuspeed * 100.f + 0.5f;
		}
		break;
	case MENUOP_SET:
		g_PlayerExtCfg[0].radialmenuspeed = (f32)data->slider.value / 100.f;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%.2f", (f32)data->slider.value / 100.f);
	}

	return 0;
}

struct menuitem g_ExtendedMouseMenuItems[] = {
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Mouse Enabled",
		0,
		menuhandlerMouseEnabled,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Mouse Aim Lock",
		0,
		menuhandlerMouseAimLock,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Mouse Lock Mode",
		0,
		menuhandlerMouseLockMode,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Mouse Menu Navigation",
		0,
		menuhandlerMenuMouseControl,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Mouse Speed X",
		3000,
		menuhandlerMouseSpeedX,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Mouse Speed Y",
		3000,
		menuhandlerMouseSpeedY,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Crosshair Speed X",
		1000,
		menuhandlerMouseAimSpeedX,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Crosshair Speed Y",
		1000,
		menuhandlerMouseAimSpeedY,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Radial Menu Speed",
		1000,
		menuhandlerRadialMenuSpeed,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedMouseMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Extended Mouse Options",
	g_ExtendedMouseMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerStickSpeed(s32 operation, struct menuitem *item, union handlerdata *data);
static MenuItemHandlerResult menuhandlerStickDeadzone(s32 operation, struct menuitem *item, union handlerdata *data);

struct menuitem g_ExtendedStickMenuItems[] = {
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"LStick Scale X",
		20,
		menuhandlerStickSpeed,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"LStick Scale Y",
		20,
		menuhandlerStickSpeed,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"RStick Scale X",
		20,
		menuhandlerStickSpeed,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"RStick Scale Y",
		20,
		menuhandlerStickSpeed,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"LStick Deadzone X",
		32,
		menuhandlerStickDeadzone,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"LStick Deadzone Y",
		32,
		menuhandlerStickDeadzone,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"RStick Deadzone X",
		32,
		menuhandlerStickDeadzone,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"RStick Deadzone Y",
		32,
		menuhandlerStickDeadzone,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static MenuItemHandlerResult menuhandlerStickSpeed(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const s32 idx = item - g_ExtendedStickMenuItems;
	const s32 stick = idx / 2;
	const s32 axis = idx % 2;

	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = inputControllerGetAxisScale(g_ExtMenuPlayer, stick, axis) * 10.f + 0.5f;
		break;
	case MENUOP_SET:
		inputControllerSetAxisScale(g_ExtMenuPlayer, stick, axis, (f32)data->slider.value / 10.f);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerStickDeadzone(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const s32 idx = item - (g_ExtendedStickMenuItems + 5);
	const s32 stick = idx / 2;
	const s32 axis = idx % 2;

	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = inputControllerGetAxisDeadzone(g_ExtMenuPlayer, stick, axis) * 32.f + 0.5f;
		break;
	case MENUOP_SET:
		inputControllerSetAxisDeadzone(g_ExtMenuPlayer, stick, axis, (f32)data->slider.value / 32.f);
		break;
	}

	return 0;
}

struct menudialogdef g_ExtendedStickMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Analog Stick Settings",
	g_ExtendedStickMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerVibration(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = inputRumbleGetStrength(g_ExtMenuPlayer) * 10.f + 0.5f;
		break;
	case MENUOP_SET:
		inputRumbleSetStrength(g_ExtMenuPlayer, (f32)data->slider.value / 10.f);
		break;
	case MENUOP_CHECKHIDDEN:
	case MENUOP_CHECKDISABLED:
		if (!inputRumbleSupported(g_ExtMenuPlayer)) {
			return true;
		}
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerAnalogMovement(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return inputControllerGetDualAnalog(g_ExtMenuPlayer);
	case MENUOP_SET:
		inputControllerSetDualAnalog(g_ExtMenuPlayer, data->checkbox.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerSwapSticks(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return inputControllerGetSticksSwapped(g_ExtMenuPlayer);
	case MENUOP_SET:
		inputControllerSetSticksSwapped(g_ExtMenuPlayer, data->checkbox.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerController(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char ctrlname[35];
	s32 ctrls[INPUT_MAX_CONNECTED_CONTROLLERS];
	const s32 numCtrls = inputGetConnectedControllers(ctrls);
	const s32 curCtrl = inputGetAssignedControllerId(g_ExtMenuPlayer);

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = numCtrls + 1; // first option is "None"
		break;
	case MENUOP_GETOPTIONTEXT:
		if (data->dropdown.value) {
			const s32 jid = ctrls[data->dropdown.value - 1];
			const char *name = inputGetConnectedControllerName(jid);
			strncpy(ctrlname, name, sizeof(ctrlname) - 1);
			return (intptr_t)ctrlname;
		} else {
			return (intptr_t)"None";
		}
	case MENUOP_SET:
		if (data->dropdown.value == 0) {
			// unassign controller
			inputAssignController(g_ExtMenuPlayer, -1);
			joyReset();
		} else if (data->dropdown.value <= numCtrls) {
			inputAssignController(g_ExtMenuPlayer, ctrls[data->dropdown.value - 1]);
			joyReset();
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		for (s32 i = 0; i < numCtrls; ++i) {
			if (curCtrl == ctrls[i]) {
				data->dropdown.value = i + 1;
				return 0;
			}
		}
		data->dropdown.value = 0;
		break;
	}

	return 0;
}

struct menuitem g_ExtendedControllerMenuItems[] = {
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Controller",
		0,
		menuhandlerController,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Analog Movement",
		0,
		menuhandlerAnalogMovement,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Swap Sticks",
		0,
		menuhandlerSwapSticks,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Stick Settings...\n",
		0,
		(void *)&g_ExtendedStickMenuDialog,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Vibration",
		10,
		menuhandlerVibration,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static char g_ExtendedControllerMenuTitle[] = "Player 1 Controller Options";
struct menudialogdef g_ExtendedControllerMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)g_ExtendedControllerMenuTitle,
	g_ExtendedControllerMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerFullScreen(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return videoGetFullscreen();
	case MENUOP_SET:
		videoSetFullscreen(data->checkbox.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerFullScreenMode(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = {
		"Borderless",
		"Exclusive"
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		videoSetFullscreenMode(data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = videoGetFullscreenMode();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCenterWindow(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return videoGetCenterWindow();
	case MENUOP_SET:
		videoSetCenterWindow(data->checkbox.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerVsync(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const s32 numOpts = 10;
	static const char *constOpts[] = {
		"Adaptive",
		"Off",
		"On"
	};
	static char dynOpt[20];
	s32 vblanks;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = numOpts;
		break;
	case MENUOP_GETOPTIONTEXT:
		if (data->dropdown.value < ARRAYCOUNT(constOpts))
			return (intptr_t)constOpts[data->dropdown.value];
		vblanks = (s32)data->dropdown.value - 1;
		snprintf(dynOpt, sizeof(dynOpt), "On (%d frames)", vblanks);
		return (intptr_t)dynOpt;
	case MENUOP_SET:
		videoSetVsync(data->dropdown.value - 1);
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = videoGetVsync() + 1;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerFramerateLimit(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = videoGetFramerateLimit();
		break;
	case MENUOP_SET:
		videoSetFramerateLimit(data->slider.value);
		break;
	case MENUOP_GETSLIDERLABEL:
		// NOTE: data->slider.label length must not exceed 15.
		if (data->slider.value == 0) {
			strcpy(data->slider.label, "Off");
		} else {
			sprintf(data->slider.label, "%d FPS", data->slider.value);
		}
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMSAA(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 msaa;
	s32 count;
	static const char *opts[] = {
		"Off",
		"2x (MSAA)",
		"4x (MSAA)",
		"8x (MSAA)",
		"16x (MSAA)"
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		// Only the levels this GPU can build: a sample count past GL_MAX_SAMPLES
		// gives an incomplete framebuffer and a black screen.
		count = 1;
		while (count < ARRAYCOUNT(opts) && (1 << count) <= videoGetMaxMSAA()) {
			count++;
		}
		data->dropdown.value = count;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		videoSetMSAA(1 << data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		msaa = videoGetMSAA();
		if (msaa < 2) {
			data->dropdown.value = 0;
		} else if (msaa < 4) {
			data->dropdown.value = 1;
		} else if (msaa < 8) {
			data->dropdown.value = 2;
		} else if (msaa < 16) {
			data->dropdown.value = 3;
		} else {
			data->dropdown.value = 4;
		}
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerResolution(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static char resstring[32];
	static const char *rescustom = "Custom";
	displaymode mode;

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		if (videoGetFullscreen() && videoGetFullscreenMode() == 0) {
			return true;
		}
		break;
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = videoGetNumDisplayModes();
		break;
	case MENUOP_GETOPTIONTEXT:
		videoGetDisplayMode(&mode, data->dropdown.value);
		if (mode.width == 0 && mode.height == 0) {
			return (intptr_t)rescustom;
		} else {
			snprintf(resstring, sizeof(resstring), "%dx%d", mode.width, mode.height);
		}
		return (intptr_t)resstring;
	case MENUOP_SET:
		videoSetDisplayMode(data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = videoGetDisplayModeIndex();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerTexFilter(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = {
		"Nearest",
		"Bilinear",
		"Three Point"
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		videoSetTextureFilter(data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = videoGetTextureFilter();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerTexDetail(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return (videoGetDetailTextures() != 0);
	case MENUOP_SET:
		videoSetDetailTextures(data->checkbox.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerTexFilter2D(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return videoGetTextureFilter2D();
	case MENUOP_SET:
		videoSetTextureFilter2D(data->checkbox.value);
		g_TexFilter2D = videoGetTextureFilter2D() ? G_TF_BILERP : G_TF_POINT;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerAnisotropicFiltering(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = videoGetAnisotropicFilter();
		break;
	case MENUOP_SET:
		videoSetAnisotropicFilter(data->slider.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerDisplayFPS(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return videoGetDisplayFPS();
	case MENUOP_SET:
		videoSetDisplayFPS(data->checkbox.value);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGeMuzzleFlashes(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_BgunGeMuzzleFlashes;
	case MENUOP_SET:
		g_BgunGeMuzzleFlashes = data->checkbox.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerUncapTickrate(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return (g_TickRateDiv == 0);
	case MENUOP_SET:
		g_TickRateDiv = !data->checkbox.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCenterHUD(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = {
		"None",
		"4:3",
		"Wide"
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_HudCenter = data->checkbox.value;
		if (g_HudCenter == HUDCENTER_NORMAL) {
			g_HudAlignModeL = G_ASPECT_CENTER_EXT;
			g_HudAlignModeR = G_ASPECT_CENTER_EXT;
		} else if (g_HudCenter == HUDCENTER_WIDE) {
			g_HudAlignModeL = G_ASPECT_LEFT_EXT | G_ASPECT_WIDE_EXT;
			g_HudAlignModeR = G_ASPECT_RIGHT_EXT | G_ASPECT_WIDE_EXT;
		}	else {
			g_HudAlignModeL = G_ASPECT_LEFT_EXT;
			g_HudAlignModeR = G_ASPECT_RIGHT_EXT;
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_HudCenter;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerScreenShake(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = g_ViShakeIntensityMult * 10.f + 0.5f;
		break;
	case MENUOP_SET:
		g_ViShakeIntensityMult = (f32)data->slider.value / 10.f;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerGlareBrightness(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = videoGetGlareBrightness() * 10.f + 0.5f;
		break;
	case MENUOP_SET:
		videoSetGlareBrightness((f32)data->slider.value / 10.f);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerOverexposureScale(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = videoGetOverexposureScale() * 10.f + 0.5f;
		break;
	case MENUOP_SET:
		videoSetOverexposureScale((f32)data->slider.value / 10.f);
		break;
	}

	return 0;
}

struct menuitem g_ExtendedVideoMenuItems[] = {
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Full Screen",
		0,
		menuhandlerFullScreen,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Full Screen Mode",
		0,
		menuhandlerFullScreenMode,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Resolution",
		0,
		menuhandlerResolution,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Center Window",
		0,
		menuhandlerCenterWindow,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Anti-aliasing",
		0,
		menuhandlerMSAA,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Vsync",
		0,
		menuhandlerVsync,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE | MENUITEMFLAG_SLIDER_DEFERRED,
		(uintptr_t)"Framerate Limit",
		VIDEO_MAX_FPS,
		menuhandlerFramerateLimit,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Uncap Tickrate",
		0,
		menuhandlerUncapTickrate,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Display FPS",
		0,
		menuhandlerDisplayFPS,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Texture Filtering",
		0,
		menuhandlerTexFilter,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"GUI Texture Filtering",
		0,
		menuhandlerTexFilter2D,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Anisotropic Filtering",
		8,
		menuhandlerAnisotropicFiltering,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Detail Textures",
		0,
		menuhandlerTexDetail,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"HUD Centering",
		0,
		menuhandlerCenterHUD,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"GE64-style Muzzle Flashes",
		0,
		menuhandlerGeMuzzleFlashes,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Explosion Shake",
		20,
		menuhandlerScreenShake,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Glare Brightness",
		10,
		menuhandlerGlareBrightness,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Overexposure Scale",
		10,
		menuhandlerOverexposureScale,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedVideoMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Extended Video Options",
	g_ExtendedVideoMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerDisableMpDeathMusic(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_MusicDisableMpDeath;
	case MENUOP_SET:
		g_MusicDisableMpDeath = data->checkbox.value;
		break;
	}

	return 0;
}

struct menuitem g_ExtendedAudioMenuItems[] = {
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Disable MP Death Music",
		0,
		menuhandlerDisableMpDeathMusic,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedAudioMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Extended Audio Options",
	g_ExtendedAudioMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerUseKeyReloads(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !g_PlayerExtCfg[g_ExtMenuPlayer].extcontrols;
	case MENUOP_GET:
		return g_PlayerExtCfg[g_ExtMenuPlayer].usereloads;
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].usereloads = data->checkbox.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrouchMode(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = {
		"Hold",
		"Analog",
		"Toggle",
		"Toggle + Analog"
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].crouchmode = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_PlayerExtCfg[g_ExtMenuPlayer].crouchmode;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerFieldOfView(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = g_PlayerExtCfg[g_ExtMenuPlayer].fovy + 0.5f;
		break;
	case MENUOP_SET:
		if (data->slider.value >= 15) {
			g_PlayerExtCfg[g_ExtMenuPlayer].fovy = data->slider.value;
			if (g_PlayerExtCfg[g_ExtMenuPlayer].fovzoom) {
				g_PlayerExtCfg[g_ExtMenuPlayer].fovzoommult = g_PlayerExtCfg[g_ExtMenuPlayer].fovy / 60.f;
				playerClampGunZoomFovY(g_ExtMenuPlayer);
			}
		}
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairSway(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = g_PlayerExtCfg[g_ExtMenuPlayer].crosshairsway * 10.f + 0.5f;
		break;
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].crosshairsway = (f32)data->slider.value / 10.f;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairEdgeBoundary(s32 operation, struct menuitem* item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = (s32)(g_PlayerExtCfg[g_ExtMenuPlayer].crosshairedgeboundary * 10.f + 0.5f);
		break;
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].crosshairedgeboundary = (f32)data->slider.value / 10.f;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%d", (s32)data->slider.value);
		break;
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairR(s32 operation, struct menuitem* item, union handlerdata* data)
{
	u32 newColor;

	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = (g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour >> 24) & 0xFF;
		break;

	case MENUOP_SET:
		newColor = (g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour & 0xFFFFFF) | data->slider.value << 24;
		g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour = newColor;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairG(s32 operation, struct menuitem* item, union handlerdata* data)
{
	u32 newColor;

	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = (g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour >> 16) & 0xFF;
		break;

	case MENUOP_SET:
		newColor = (g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour & 0xFF00FFFF) | data->slider.value << 16;
		g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour = newColor;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairB(s32 operation, struct menuitem* item, union handlerdata* data)
{
	u32 newColor;

	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = (g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour >> 8) & 0xFF;
		break;

	case MENUOP_SET:
		newColor = (g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour & 0xFFFF00FF) | data->slider.value << 8;
		g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour = newColor;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairA(s32 operation, struct menuitem* item, union handlerdata* data)
{
	u32 newColor;

	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour & 0xFF;
		break;

	case MENUOP_SET:
		newColor = (g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour & 0xFFFFFF00) | data->slider.value;
		g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour = newColor;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairColorPreview(s32 operation, struct menuitem* item, union handlerdata* data)
{
	if (operation == MENUOP_GETCOLOUR) {
		data->label.colour1 = g_PlayerExtCfg[g_ExtMenuPlayer].crosshaircolour;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairSize(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = g_PlayerExtCfg[g_ExtMenuPlayer].crosshairsize;
		break;
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].crosshairsize = data->slider.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerCrosshairHealth(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = {
		"Off",
		"On (Green)",
		"On (White)"
	};

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_PlayerExtCfg[g_ExtMenuPlayer].crosshairhealth = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_PlayerExtCfg[g_ExtMenuPlayer].crosshairhealth;
	}

	return 0;
}

struct menuitem g_ExtendedGameCrosshairColourMenuItems[] = {
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Red",
		255,
		menuhandlerCrosshairR,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Green",
		255,
		menuhandlerCrosshairG,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Blue",
		255,
		menuhandlerCrosshairB,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Alpha",
		255,
		menuhandlerCrosshairA,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_COLORBOX,
		0,
		0,
		0,
		0,
		menuhandlerCrosshairColorPreview,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedGameCrosshairColourMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Crosshair Colour",
	g_ExtendedGameCrosshairColourMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

struct menuitem g_ExtendedGameMenuItems[] = {
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Crouch Mode",
		0,
		menuhandlerCrouchMode,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Vert FOV",
		170,
		menuhandlerFieldOfView,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Crosshair Sway",
		20,
		menuhandlerCrosshairSway,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Crosshair Edge Deadzone",
		10,
		menuhandlerCrosshairEdgeBoundary,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Crosshair Size",
		4,
		menuhandlerCrosshairSize,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SELECTABLE_OPENSDIALOG,
		(uintptr_t)"Crosshair Colour\n",
		0,
		(void*)&g_ExtendedGameCrosshairColourMenuDialog,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Crosshair Colour by Health",
		0,
		menuhandlerCrosshairHealth,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Use Key Reloads",
		0,
		menuhandlerUseKeyReloads,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static char g_ExtendedGameMenuTitle[] = "Player 1 Game Options";
struct menudialogdef g_ExtendedGameMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)g_ExtendedGameMenuTitle,
	g_ExtendedGameMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerDoBind(s32 operation, struct menuitem *item, union handlerdata *data);

struct menuitem g_ExtendedBindKeyMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Press new key or button...\n",
		0,
		menuhandlerDoBind,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CENTRE | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"ESC to cancel, DEL to remove binding\n",
		0,
		menuhandlerDoBind,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedBindKeyMenuDialog = {
	MENUDIALOGTYPE_SUCCESS,
	(uintptr_t)"Bind",
	g_ExtendedBindKeyMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_IGNOREBACK | MENUDIALOGFLAG_STARTSELECTS,
	NULL,
};

struct menubind {
	u32 ck;
	const char *name;
	const char *n64name;
};

static const struct menubind menuBinds[] = {
	{ CK_ZTRIG,  "Fire [ZT]\n",         "N64 Z Trigger\n" },
	{ CK_LTRIG,  "Fire Mode [LT]\n",    "N64 L Trigger\n"},
	{ CK_RTRIG,  "Aim Mode [RT]\n",     "N64 R Trigger\n" },
	{ CK_A,      "Use / Accept [A]\n",  "N64 A Button\n" },
	{ CK_B,      "Use / Cancel [B]\n",  "N64 B Button\n" },
	{ CK_START,  "Pause Menu [ST]\n",   "N64 Start\n" },
	{ CK_DPAD_U, "D-Pad Up [DU]\n",     "N64 D-Pad Up\n" },
	{ CK_DPAD_R, "D-Pad Right [DR]\n",  "N64 D-Pad Right\n" },
	{ CK_DPAD_L, "Prev Weapon [DL]\n",  "N64 D-Pad Left\n" },
	{ CK_DPAD_D, "Radial Menu [DD]\n",  "N64 D-Pad Down\n" },
	{ CK_C_U,    "Forward [CU]\n",      "N64 C-Up\n" },
	{ CK_C_D,    "Backward [CD]\n",     "N64 C-Down\n" },
	{ CK_C_R,    "Strafe Right [CR]\n", "N64 C-Right\n" },
	{ CK_C_L,    "Strafe Left [CL]\n",  "N64 C-Left\n" },
	{ CK_X,      "Reload [X]\n",        "N64 Ext X\n" },
	{ CK_Y,      "Next Weapon [Y]\n",   "N64 Ext Y\n" },
	{ CK_8000,   "Cycle Crouch [+]\n",  "N64 Ext 8000\n" },
	{ CK_4000,   "Half Crouch [+]\n",   "N64 Ext 4000\n" },
	{ CK_2000,   "Full Crouch [+]\n",   "N64 Ext 2000\n" },
	{ CK_0040,   "Fire Left [+]\n",     "N64 Ext 0040\n" },
	{ CK_0080,   "Fire Mode Left [+]\n", "N64 Ext 0080\n" },
	// Third Person and Combat Roll are not here. They belong to this fork, so
	// they are bound from Dab's Mod Options with the settings they drive.
	{ CK_ACCEPT, "UI Accept [+]\n",     "EXT UI Accept\n" },
	{ CK_CANCEL, "UI Cancel [+]\n",     "EXT UI Cancel\n" },
};

static const char *menutextBind(struct menuitem *item);
static MenuItemHandlerResult menuhandlerBind(s32 operation, struct menuitem *item, union handlerdata *data);
static MenuItemHandlerResult menuhandlerResetBindsPC(s32 operation, struct menuitem *item, union handlerdata *data);
static MenuItemHandlerResult menuhandlerResetBindsN64(s32 operation, struct menuitem *item, union handlerdata *data);

#define DEFINE_MENU_BIND() \
	{ \
		MENUITEMTYPE_DROPDOWN, \
		0, \
		0, \
		(uintptr_t)menutextBind, \
		0, \
		menuhandlerBind, \
	}

struct menuitem g_ExtendedBindsMenuItems[] = {
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	DEFINE_MENU_BIND(),
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Reset to PC Defaults\n",
		0,
		menuhandlerResetBindsPC,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Reset to N64 Defaults\n",
		0,
		menuhandlerResetBindsN64,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

static MenuItemHandlerResult menuhandlerDoBind(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (!menuIsDialogOpen(&g_ExtendedBindKeyMenuDialog)) {
		return 0;
	}

	if (inputKeyPressed(VK_ESCAPE)) {
		menuPopDialog();
		return 0;
	}

	const s32 key = inputGetLastKey();
	if (key && key != VK_ESCAPE) {
		const s32 vk = (key == VK_DELETE) ? 0 : key;
		if (g_BindKeySetter) {
			g_BindKeySetter(vk);
		} else {
			inputKeyBind(g_ExtMenuPlayer, g_BindContKey, g_BindIndex, vk);
		}
		menuPopDialog();
	}

	return 0;
}

static const char *menutextBind(struct menuitem *item)
{
	return g_PlayerExtCfg[g_ExtMenuPlayer].extcontrols ?
		menuBinds[item - g_ExtendedBindsMenuItems].name :
		menuBinds[item - g_ExtendedBindsMenuItems].n64name;
}

static MenuItemHandlerResult menuhandlerBind(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const s32 idx = item - g_ExtendedBindsMenuItems;
	const u32 *binds;

	static char keyname[128];

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = INPUT_MAX_BINDS;
		break;
	case MENUOP_GETOPTIONTEXT:
		binds = inputKeyGetBinds(g_ExtMenuPlayer, menuBinds[idx].ck);
		if (binds && binds[data->dropdown.value]) {
			strncpy(keyname, inputGetKeyName(binds[data->dropdown.value]), sizeof(keyname) - 1);
			for (char *p = keyname; *p; ++p) {
				if (*p == '_') *p = ' ';
			}
			return (intptr_t)keyname;
		}
		return (intptr_t)"NONE";
	case MENUOP_SET:
		g_ExtendedBindKeyMenuItems[0].param2 = (uintptr_t)menuBinds[idx].name;
		g_BindIndex = data->dropdown.value;
		g_BindContKey = menuBinds[idx].ck;
		g_BindKeySetter = NULL;
		inputClearLastKey();
		menuPushDialog(&g_ExtendedBindKeyMenuDialog);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = 0;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerResetBindsPC(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		inputSetDefaultKeyBinds(g_ExtMenuPlayer, false);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerResetBindsN64(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		inputSetDefaultKeyBinds(g_ExtMenuPlayer, true);
	}

	return 0;
}

static char g_ExtendedBindsMenuTitle[] = "Player 1 Bindings";
struct menudialogdef g_ExtendedBindsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)g_ExtendedBindsMenuTitle,
	g_ExtendedBindsMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT | MENUDIALOGFLAG_STARTSELECTS | MENUDIALOGFLAG_IGNOREBACK,
	NULL,
};

static MenuItemHandlerResult menuhandlerOpenControllerMenu(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_ExtNextDialog = &g_ExtendedControllerMenuDialog;
		menuPushDialog(&g_ExtendedSelectPlayerMenuDialog);
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerOpenGameMenu(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_ExtNextDialog = &g_ExtendedGameMenuDialog;
		menuPushDialog(&g_ExtendedSelectPlayerMenuDialog);
	}
	return 0;
}

static MenuItemHandlerResult menuhandlerOpenBindsMenu(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		g_ExtNextDialog = &g_ExtendedBindsMenuDialog;
		menuPushDialog(&g_ExtendedSelectPlayerMenuDialog);
	}
	return 0;
}

/**
 * Settings Preset: the fork's additions turned on or off as a set.
 *
 * Vanilla is stock Perfect Dark with the fixes on; Dab's Settings is what the
 * fork shipped as its defaults until 2026-09-09; Ghost Trials is Vanilla with
 * the time trial racing. Custom is any other mix, and is what the dropdown
 * reads once a single setting is changed away from a preset - the preset is
 * applied on selection and remembered by nothing, so the settings themselves
 * are still the truth and a preset never has to be re-read.
 *
 * A preset covers what changes how the game plays and looks: movement, the
 * tilt, the bodies, the picture, and the settings that are a way of playing
 * (Start Armed, Guards Alerted!, Akimbo, Mission Respawn), which every preset
 * turns off since none of the three is a way of playing. It leaves alone the
 * third person camera, which has a preset of its own and which a player
 * showcasing the fork sets up to taste; the key binds, since Akimbo Triggers
 * rewrites them; and the Randomizer, the spectator and the recorder, which
 * are modes and tools rather than settings.
 */
struct modpreset {
	const char *name;
	s32 jumpheight;
	s32 roll;
	s32 melee;
	s32 flinch;
	s32 cameratilt;
	s32 tiltforward;
	s32 gunsway;
	s32 bodies;
	s32 bodytime;
	s32 bodiesdrawn;
	s32 codaiming;
	s32 explosionshake;
	s32 tranqeffect;
	s32 cleantext;
	s32 smoothtext;
	s32 enhancetextures;
	s32 vividcolours;
	s32 blacklevel;
	s32 modellod;
	s32 ghostmode;
	s32 ghostsplits;
};

#define MODPRESET_CUSTOM 0

static const struct modpreset g_ModPresets[] = {
	//  name              jump  roll              melee  flinch  tilt            fwd    sway  bodies  time  drawn  cod    shake  tranq  clean  smooth  enhance        vivid          black          lod   ghost            splits
	{ "Custom",           0,    0,                0,     0,      0,              0,     0,    0,      0,    0,     0,     0,     0,     0,     0,      0,             0,             0,             0,    0,               0     },
	{ "Vanilla",          0,    MODROLL_OFF,      false, false,  MODTILT_OFF,    false, true, 0,      0,    64,    false, false, true,  true,  true,   MODENHANCE_OFF, MODVIVID_OFF,  MODBLACK_OFF,  true, MODGHOST_OFF,    true  },
	{ "Dab's Settings",   1,    MODROLL_EVERYONE, true,  true,   MODTILT_NORMAL, false, true, 128,    0,    64,    false, false, true,  true,  true,   MODENHANCE_2X, MODVIVID_LIGHT, MODBLACK_LIGHT, true, MODGHOST_OFF,    true  },
	{ "Ghost Trials",     0,    MODROLL_OFF,      false, false,  MODTILT_OFF,    false, true, 0,      0,    64,    false, false, true,  true,  true,   MODENHANCE_OFF, MODVIVID_OFF,  MODBLACK_OFF,  true, MODGHOST_RACE,   true  },
};

static void menuhandlerModPresetApply(const struct modpreset *preset)
{
	g_ModOptions.jumpheight = preset->jumpheight;
	g_ModOptions.roll = preset->roll;
	g_ModOptions.melee = preset->melee;
	g_ModOptions.flinch = preset->flinch;
	g_ModOptions.cameratilt = preset->cameratilt;
	g_ModOptions.tiltforward = preset->tiltforward;
	g_ModOptions.gunsway = preset->gunsway;
	g_ModOptions.bodies = preset->bodies;
	g_ModOptions.bodytime = preset->bodytime;
	g_ModOptions.bodiesdrawn = preset->bodiesdrawn;
	g_ModOptions.codaiming = preset->codaiming;
	g_ModOptions.explosionshake = preset->explosionshake;
	g_ModOptions.tranqeffect = preset->tranqeffect;
	g_ModOptions.cleantext = preset->cleantext;
	g_ModOptions.smoothtext = preset->smoothtext;
	g_ModOptions.enhancetextures = preset->enhancetextures;
	g_ModOptions.vividcolours = preset->vividcolours;
	g_ModOptions.blacklevel = preset->blacklevel;
	g_ModOptions.modellod = preset->modellod;
	g_ModGhostMode = preset->ghostmode;
	g_ModGhostSplits = preset->ghostsplits;

	// The ways of playing, off in every preset.
	g_ModOptions.spawnweapon = SPAWNWEAPON_OFF;
	g_ModOptions.guardsalerted = MODALARM_OFF;
	g_ModOptions.akimbo = MODAKIMBO_OFF;
	g_ModOptions.missionrespawn = false;

	// What the individual setters would have told the renderer.
	videoSetCleanTextOutlines(g_ModOptions.cleantext);
	videoSetTextureEnhance(modGetTextureEnhanceScale(), modGetSmoothTextScale());
	videoSetVividColours(modGetVividSaturation(), modGetVividContrast());
	videoSetBlackLevel(modGetBlackLevelLift());
}

static bool menuhandlerModPresetMatches(const struct modpreset *preset)
{
	return g_ModOptions.jumpheight == preset->jumpheight
		&& g_ModOptions.roll == preset->roll
		&& g_ModOptions.melee == preset->melee
		&& g_ModOptions.flinch == preset->flinch
		&& g_ModOptions.cameratilt == preset->cameratilt
		&& g_ModOptions.tiltforward == preset->tiltforward
		&& g_ModOptions.gunsway == preset->gunsway
		&& g_ModOptions.bodies == preset->bodies
		&& g_ModOptions.bodytime == preset->bodytime
		&& g_ModOptions.bodiesdrawn == preset->bodiesdrawn
		&& g_ModOptions.codaiming == preset->codaiming
		&& g_ModOptions.explosionshake == preset->explosionshake
		&& g_ModOptions.tranqeffect == preset->tranqeffect
		&& g_ModOptions.cleantext == preset->cleantext
		&& g_ModOptions.smoothtext == preset->smoothtext
		&& g_ModOptions.enhancetextures == preset->enhancetextures
		&& g_ModOptions.vividcolours == preset->vividcolours
		&& g_ModOptions.blacklevel == preset->blacklevel
		&& g_ModOptions.modellod == preset->modellod
		&& g_ModGhostMode == preset->ghostmode
		&& g_ModGhostSplits == preset->ghostsplits
		&& g_ModOptions.spawnweapon == SPAWNWEAPON_OFF
		&& g_ModOptions.guardsalerted == MODALARM_OFF
		&& g_ModOptions.akimbo == MODAKIMBO_OFF
		&& g_ModOptions.missionrespawn == false;
}

static MenuItemHandlerResult menuhandlerModPreset(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 i;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(g_ModPresets);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)g_ModPresets[data->dropdown.value].name;
	case MENUOP_SET:
		i = data->dropdown.value;

		if (i != MODPRESET_CUSTOM && i < (s32)ARRAYCOUNT(g_ModPresets)) {
			menuhandlerModPresetApply(&g_ModPresets[i]);
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = MODPRESET_CUSTOM;

		for (i = MODPRESET_CUSTOM + 1; i < (s32)ARRAYCOUNT(g_ModPresets); i++) {
			if (menuhandlerModPresetMatches(&g_ModPresets[i])) {
				data->dropdown.value = i;
				break;
			}
		}
	}

	return 0;
}

/**
 * Dab's Mod Options - the fork's own settings, all in one page.
 *
 * Jump and Start Armed were arena rules in mpsetup.options until they moved
 * here. They are global now, kept in pd.ini with the rest of the port's
 * settings, which is what lets the jump work in a solo mission: an arena rule
 * only exists while a Combat Sim match does.
 *
 * The two key binds this fork added live here as well rather than on the Key
 * Bindings page, next to the settings they drive.
 */
static MenuItemHandlerResult menuhandlerModJump(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[JUMPHEIGHT_MAX + 1] = { "Off", "1x", "2x", "3x", "4x", "5x" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.jumpheight = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = modGetJumpHeight();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerModJumpFor(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Everyone", "Players Only" };

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !modIsJumpEnabled();
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.jumpwho = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ModOptions.jumpwho;
	}

	return 0;
}

/**
 * Who Start Armed applies to. Greyed out while Start Armed is off, the way Jump
 * For is greyed out while Jump is off.
 */
static MenuItemHandlerResult menuhandlerModStartArmedFor(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Everyone", "Players Only" };

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return modGetSpawnWeapon() == SPAWNWEAPON_OFF;
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.spawnweaponwho = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ModOptions.spawnweaponwho;
	}

	return 0;
}

/**
 * Mission Respawn: a death in a mission is a new life where the player
 * fell, not Mission Failed. Lives is under it. See modrespawn.c.
 */
static MenuItemHandlerResult menuhandlerModMissionRespawn(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModOptions.missionrespawn;
	case MENUOP_SET:
		g_ModOptions.missionrespawn = data->checkbox.value;
		break;
	}

	return 0;
}

/**
 * How many lives a mission has in all: unlimited, or five at a time up to
 * fifty. Greyed out while Mission Respawn is off, the way the guard
 * settings are under Guards Alerted!.
 */
static MenuItemHandlerResult menuhandlerModMissionLives(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Unlimited", "5", "10", "15", "20", "25", "30", "35", "40", "45", "50" };

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return g_ModOptions.missionrespawn == 0;
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.missionlives = data->dropdown.value * MODLIVES_STEP;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = modGetMissionLives() / MODLIVES_STEP;
	}

	return 0;
}

/**
 * Guards Alerted!: the alarm never stops and guards keep coming, in every
 * mode. How many at once and how fast are the two items under it; the siren
 * is its own checkbox, since a whole match of it is a different thing from
 * thirty seconds. See modalarm.c.
 */
static MenuItemHandlerResult menuhandlerModGuardsAlerted(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModOptions.guardsalerted != MODALARM_OFF;
	case MENUOP_SET:
		g_ModOptions.guardsalerted = data->checkbox.value ? MODALARM_ON : MODALARM_OFF;
		break;
	}

	return 0;
}

/**
 * Guards per ten seconds. The slider runs from one, a guard every ten
 * seconds, to fifty, one every fifth of a second - the whole eighty inside
 * twenty seconds.
 */
static MenuItemHandlerResult menuhandlerModGuardSpawnSpeed(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return g_ModOptions.guardsalerted == MODALARM_OFF;
	case MENUOP_GETSLIDER:
		data->slider.value = modGetGuardSpawnSpeed() - MODALARM_SPEED_MIN;
		break;
	case MENUOP_SET:
		g_ModOptions.guardspawnspeed = data->slider.value + MODALARM_SPEED_MIN;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%d", (s32)data->slider.value + MODALARM_SPEED_MIN);
		break;
	}

	return 0;
}

/**
 * How many are up at once. The steps are the simulant count's kind of
 * steps, and the top is the simulant count's top.
 */
static const s32 g_ModAlertedGuardsValues[] = { 1, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64, MODALARM_GUARDS_MAX };

static s32 menuhandlerModPresetIndex(const s32 *values, s32 count, s32 value);

static MenuItemHandlerResult menuhandlerModAlertedGuards(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "1", "2", "4", "6", "8", "12", "16", "24", "32", "48", "64", "80" };

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return g_ModOptions.guardsalerted == MODALARM_OFF;
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.alertedguards = g_ModAlertedGuardsValues[data->dropdown.value];
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = menuhandlerModPresetIndex(g_ModAlertedGuardsValues,
				ARRAYCOUNT(g_ModAlertedGuardsValues), modGetAlertedGuards());
	}

	return 0;
}

/**
 * What a guard carries: the match's slots or the mission side arms, or a
 * roll of the whole table, like Start Armed's Random.
 */
static MenuItemHandlerResult menuhandlerModGuardWeapons(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Stage Weapons", "Random" };

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return g_ModOptions.guardsalerted == MODALARM_OFF;
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.guardweapons = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = modGetGuardWeapons();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerModAlarmSound(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return g_ModOptions.guardsalerted == MODALARM_OFF;
	case MENUOP_GET:
		return g_ModOptions.alarmsound;
	case MENUOP_SET:
		g_ModOptions.alarmsound = data->checkbox.value;
		break;
	}

	return 0;
}

/**
 * Akimbo: two of the gun for whoever spawns armed, when it can be held in
 * each hand. Start Armed's players and simulants, the alerted guards, or
 * both.
 */
static MenuItemHandlerResult menuhandlerModAkimbo(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Off", "Everyone", "Players & Sims", "Guards" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.akimbo = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ModOptions.akimbo;
	}

	return 0;
}

/**
 * Akimbo Triggers: a trigger per hand on a controller. Turning it on or off
 * rewrites the three controller binds it needs, there and then.
 */
static MenuItemHandlerResult menuhandlerModAkimboTriggers(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModOptions.akimbotriggers;
	case MENUOP_SET:
		g_ModOptions.akimbotriggers = data->checkbox.value;
		inputApplyAkimboTriggers(g_ModOptions.akimbotriggers);
		break;
	}

	return 0;
}

/**
 * Whether an explosion shakes the screen. The Video page's slider is how
 * much; this is whether.
 */
static MenuItemHandlerResult menuhandlerModExplosionShake(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModOptions.explosionshake;
	case MENUOP_SET:
		g_ModOptions.explosionshake = data->checkbox.value;
		break;
	}

	return 0;
}

/**
 * Tranquilizer Effect: the drugged screen a dart, a bolt or an N-bomb gives
 * the player. On is stock. Off is for anyone who cannot play through it, and
 * takes nothing away from the weapon: a guard the player darts still goes
 * down. See modIsTranquilizerEffectOn().
 */
static MenuItemHandlerResult menuhandlerModTranqEffect(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModOptions.tranqeffect;
	case MENUOP_SET:
		g_ModOptions.tranqeffect = data->checkbox.value;
		break;
	}

	return 0;
}

/**
 * Camera Tilt: how far the view leans into a sidestep or a look, and how
 * high it bobs with a step. An amount rather than a switch, because the
 * same motion that gives one player a sense of weight gives another a
 * headache.
 */
static MenuItemHandlerResult menuhandlerModCameraTilt(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Off", "Light", "Normal", "Heavy" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.cameratilt = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ModOptions.cameratilt;
	}

	return 0;
}

/**
 * The Randomizer's rows moved out of this page and onto its own, next to Solo
 * Missions: a mission dealt again from its own pieces, and the run that keeps
 * dealing rooms across every map, are two ways of playing rather than
 * preferences about how a mission behaves. See port/src/randommenu.c.
 */

/**
 * Invert Camera Tilt: every lean the other way about - the roll away from
 * the sidestep, the camera away from the look, the horizon back rather
 * than down into the run. The bob has no direction and is left alone.
 * Nothing to set with Camera Tilt off.
 */
static MenuItemHandlerResult menuhandlerModTiltInvert(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return g_ModOptions.cameratilt == MODTILT_OFF;
	case MENUOP_GET:
		return g_ModOptions.tiltinvert;
	case MENUOP_SET:
		g_ModOptions.tiltinvert = data->checkbox.value;
		break;
	}

	return 0;
}

/**
 * Forward And Back Tilt: a run pitches the view down into it and backing
 * away pitches it up, the lean the roll gives a sidestep on the axis the
 * roll leaves out. Nothing to set with Camera Tilt off.
 */
static MenuItemHandlerResult menuhandlerModTiltForward(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return g_ModOptions.cameratilt == MODTILT_OFF;
	case MENUOP_GET:
		return g_ModOptions.tiltforward;
	case MENUOP_SET:
		g_ModOptions.tiltforward = data->checkbox.value;
		break;
	}

	return 0;
}

/**
 * Gun Sway With Tilt: the gun's step motion scaled up by the same amount
 * as the bob, so it does not float over a bobbing world. Nothing to set
 * with Camera Tilt off.
 */
static MenuItemHandlerResult menuhandlerModGunSway(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return g_ModOptions.cameratilt == MODTILT_OFF;
	case MENUOP_GET:
		return g_ModOptions.gunsway;
	case MENUOP_SET:
		g_ModOptions.gunsway = data->checkbox.value;
		break;
	}

	return 0;
}

/**
 * COD Style Aiming: sights, a little zoom, and moving while aiming.
 */
static MenuItemHandlerResult menuhandlerModCodAiming(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModOptions.codaiming;
	case MENUOP_SET:
		g_ModOptions.codaiming = data->checkbox.value;
		break;
	}

	return 0;
}

/**
 * Aim Lock: under COD Style Aiming, the crosshair held in the centre and
 * the aim stick turning the view. Off is the game's own free crosshair.
 */
static MenuItemHandlerResult menuhandlerModCodAimLock(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return g_ModOptions.codaiming == 0;
	case MENUOP_GET:
		return g_ModOptions.codaimlock;
	case MENUOP_SET:
		g_ModOptions.codaimlock = data->checkbox.value;
		break;
	}

	return 0;
}

/**
 * Thin Text Outlines: the border of outlined text as a thin halo rather than
 * the bold one the ROM's font bakes as a filled cell. The renderer keeps its
 * own copy of the setting. Named "Clean Text Outlines" until 2026-09-12, and
 * its pd.ini key still is, so that nobody's setting is lost to the rename.
 */
static MenuItemHandlerResult menuhandlerModCleanText(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModOptions.cleantext;
	case MENUOP_SET:
		g_ModOptions.cleantext = data->checkbox.value;
		videoSetCleanTextOutlines(g_ModOptions.cleantext);
		break;
	}

	return 0;
}

/**
 * Model LOD: the game's own distance models, swapped in past a few metres.
 */
static MenuItemHandlerResult menuhandlerModModelLod(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return modIsModelLodOn();
	case MENUOP_SET:
		g_ModOptions.modellod = data->checkbox.value;
		break;
	}

	return 0;
}

/**
 * Smooth Text: the font's glyphs scaled up with their edges sharpened. The
 * renderer keeps its own copy of this and of Enhance Textures, and drops its
 * texture cache when either changes, so the switch shows at once.
 */
static MenuItemHandlerResult menuhandlerModSmoothText(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModOptions.smoothtext;
	case MENUOP_SET:
		g_ModOptions.smoothtext = data->checkbox.value;
		videoSetTextureEnhance(modGetTextureEnhanceScale(), modGetSmoothTextScale());
		break;
	}

	return 0;
}

/**
 * Enhance Textures: the game's textures scaled up on their way to the GPU.
 */
static MenuItemHandlerResult menuhandlerModEnhanceTextures(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Off", "2x", "4x", "8x" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.enhancetextures = data->dropdown.value;
		videoSetTextureEnhance(modGetTextureEnhanceScale(), modGetSmoothTextScale());
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ModOptions.enhancetextures;
	}

	return 0;
}

/**
 * Stretched Edges: what a wall shows where its texture coordinates run past
 * the tile. The N64 repeats the tile's last row or column for ever, and a
 * level built to lean on that draws a band of stretched pixels wherever a
 * surface was made larger than its texture - invisible in 32 texels of blur
 * on a CRT, a smear once the texture is sharp or replaced by a pack.
 * Mirroring folds the tile back on itself, which meets the edge exactly and
 * so cannot seam; repeating tiles it, which suits only a picture drawn to
 * tile. A fragment inside the tile samples the same texel either way, so
 * nothing that was not already stretched changes.
 */
static MenuItemHandlerResult menuhandlerModStretchedEdges(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Original", "Mirror", "Repeat" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		videoSetClampedEdgeMode(data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = videoGetClampedEdgeMode();
	}

	return 0;
}

/**
 * Vivid Colours: the finished frame's saturation and contrast turned up.
 */
static MenuItemHandlerResult menuhandlerModVividColours(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Off", "Light", "Normal", "Heavy" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.vividcolours = data->dropdown.value;
		videoSetVividColours(modGetVividSaturation(), modGetVividContrast());
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ModOptions.vividcolours;
	}

	return 0;
}

/**
 * Black Level: the floor taken off the finished frame's blacks.
 */
static MenuItemHandlerResult menuhandlerModBlackLevel(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Off", "Light", "Normal", "Heavy" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.blacklevel = data->dropdown.value;
		videoSetBlackLevel(modGetBlackLevelLift());
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ModOptions.blacklevel;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerModRoll(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Off", "Everyone", "Players Only" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.roll = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ModOptions.roll;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerModStartArmed(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Off", "First Weapon", "Random" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.spawnweapon = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = modGetSpawnWeapon();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerModMelee(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModOptions.melee;
	case MENUOP_SET:
		g_ModOptions.melee = data->checkbox.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerModFlinch(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModOptions.flinch;
	case MENUOP_SET:
		g_ModOptions.flinch = data->checkbox.value;
		break;
	}

	return 0;
}

/**
 * Bodies left lying where they fell, and how long each one lies there.
 *
 * Presets rather than sliders: the numbers worth choosing between are far
 * apart, and a slider that has to be pushed five hundred times to reach the
 * other end is not a control.
 *
 * The cap is read again at stage load, where the chr, model, anim and prop
 * slots a body needs are reserved. Raising it during a match therefore buys
 * nothing until the next one - handovers past the reserve simply fail, and
 * those bodies fade the way they used to. Lowering it takes effect at once.
 */
static const s32 g_ModBodiesValues[] = { MODBODIES_OFF, 8, 16, 32, 64, 128, 250, MODBODIES_MAX };
static const s32 g_ModBodyTimeValues[] = { MODBODYTIME_OFF, 15, 30, 60, 120, 300, MODBODYTIME_MAX };
static const s32 g_ModBodiesDrawnValues[] = { 8, 16, 32, 64, 128, 250, MODBODIESDRAWN_ALL };

static s32 menuhandlerModPresetIndex(const s32 *values, s32 count, s32 value)
{
	s32 index = 0;
	s32 i;

	for (i = 0; i < count; i++) {
		if (value >= values[i]) {
			index = i;
		}
	}

	return index;
}

static MenuItemHandlerResult menuhandlerModBodies(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Off", "8", "16", "32", "64", "128", "250", "500" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.bodies = g_ModBodiesValues[data->dropdown.value];
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = menuhandlerModPresetIndex(g_ModBodiesValues,
				ARRAYCOUNT(g_ModBodiesValues), modGetBodiesKept());
	}

	return 0;
}

/**
 * Kept and drawn are separate numbers because they cost differently: a body
 * lying in the next room is nearly free, and the same body on screen is not.
 * Drawing is where the frame goes, so this is the one to lower first.
 */
static MenuItemHandlerResult menuhandlerModBodiesDrawn(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "8", "16", "32", "64", "128", "250", "All" };

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !modKeepsBodies();
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.bodiesdrawn = g_ModBodiesDrawnValues[data->dropdown.value];
		break;
	case MENUOP_GETSELECTEDINDEX: {
		s32 drawn = modGetBodiesDrawn();
		s32 i;

		// All is last in the list but zero as a value, so it is not the
		// nearest-below match the other dropdowns look for.
		data->dropdown.value = ARRAYCOUNT(opts) - 1;

		if (drawn != MODBODIESDRAWN_ALL) {
			data->dropdown.value = 0;

			for (i = 0; i < ARRAYCOUNT(g_ModBodiesDrawnValues) - 1; i++) {
				if (drawn >= g_ModBodiesDrawnValues[i]) {
					data->dropdown.value = i;
				}
			}
		}
		break;
	}
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerModBodyTime(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Until Replaced", "15 Seconds", "30 Seconds", "1 Minute", "2 Minutes", "5 Minutes", "10 Minutes" };

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return !modKeepsBodies();
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.bodytime = g_ModBodyTimeValues[data->dropdown.value];
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = menuhandlerModPresetIndex(g_ModBodyTimeValues,
				ARRAYCOUNT(g_ModBodyTimeValues), modGetBodyTime());
	}

	return 0;
}

/**
 * The camera sliders run from the closest the camera may sit rather than from
 * zero, because a third person camera on the eye is not a third person camera.
 *
 * Sideways is the one that runs either side of zero, and zero is a value a
 * player wants to be able to land on again - centred is what the view was
 * before the setting existed. So it steps in fives rather than ones: 150 units
 * each way is sixty notches to drag through instead of three hundred, five
 * units is a twelfth of a step of Joanna's and reads as nothing, and the middle
 * of the bar is exactly centred.
 */
#define MODCAM_MINDIST  60
#define MODCAM_MAXSIDE  150
#define MODCAM_SIDESTEP 5
#define MODCAM_MAXFWD   150
#define MODCAM_FWDSTEP  5
#define MODCAM_MAXHEIGHT  150
#define MODCAM_HEIGHTSTEP 5

/**
 * The offsets as named sets, because a third person view is one combination of
 * the four and not four decisions taken separately: the shoulder view wants the
 * distance brought in and the height raised with the sideways, and a player who
 * moves one slider at a time has to find that out a slider at a time.
 *
 * Custom is not a preset and is never applied. It is the row's answer when the
 * offsets match none of them, which is what moving any slider leaves behind, so
 * the row follows the sliders rather than having to be put back by hand.
 *
 * Wall Clearance and Minimum Distance are left alone: they are what the camera
 * does about the level rather than where it is put, and a preset that quietly
 * retuned the collision would be a preset nobody could undo.
 */
struct modcampreset {
	const char *name;
	f32 dist;
	f32 side;
	f32 fwd;
	f32 height;
};

#define MODCAM_PRESET_CUSTOM 0

static const struct modcampreset g_ModCamPresets[] = {
	// name              dist  side  fwd  height
	{ "Custom",          0,    0,    0,   0   },
	{ "Default",         200,  0,    0,   0   },
	{ "Close",           100,  0,    0,   15  },
	{ "Right Shoulder",  130,  55,   0,   25  },
	{ "Left Shoulder",   130,  -55,  0,   25  },
	{ "Wide",            400,  0,    0,   50  },
	{ "Raised",          250,  0,    0,   90  },
};

/**
 * The sliders move in whole units, so anything within half of one is the value
 * the preset asked for and not a player who happened to stop nearby.
 */
static bool menuhandlerModCamPresetMatches(const struct modcampreset *preset)
{
	f32 tolerance = 0.5f;

	return g_ModOptions.camdist > preset->dist - tolerance
		&& g_ModOptions.camdist < preset->dist + tolerance
		&& g_ModOptions.camside > preset->side - tolerance
		&& g_ModOptions.camside < preset->side + tolerance
		&& g_ModOptions.camfwd > preset->fwd - tolerance
		&& g_ModOptions.camfwd < preset->fwd + tolerance
		&& g_ModOptions.camheight > preset->height - tolerance
		&& g_ModOptions.camheight < preset->height + tolerance;
}

static MenuItemHandlerResult menuhandlerModCamPreset(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 i;

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(g_ModCamPresets);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)g_ModCamPresets[data->dropdown.value].name;
	case MENUOP_SET:
		i = data->dropdown.value;

		if (i != MODCAM_PRESET_CUSTOM && i < (s32)ARRAYCOUNT(g_ModCamPresets)) {
			g_ModOptions.camdist = g_ModCamPresets[i].dist;
			g_ModOptions.camside = g_ModCamPresets[i].side;
			g_ModOptions.camfwd = g_ModCamPresets[i].fwd;
			g_ModOptions.camheight = g_ModCamPresets[i].height;
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = MODCAM_PRESET_CUSTOM;

		for (i = MODCAM_PRESET_CUSTOM + 1; i < (s32)ARRAYCOUNT(g_ModCamPresets); i++) {
			if (menuhandlerModCamPresetMatches(&g_ModCamPresets[i])) {
				data->dropdown.value = i;
				break;
			}
		}
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerModCamDist(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = (s32)(g_ModOptions.camdist + 0.5f) - MODCAM_MINDIST;
		break;
	case MENUOP_SET:
		g_ModOptions.camdist = (f32)(data->slider.value + MODCAM_MINDIST);
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%d", (s32)data->slider.value + MODCAM_MINDIST);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerModCamClearance(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = (s32)(g_ModOptions.camclearance + 0.5f);
		break;
	case MENUOP_SET:
		g_ModOptions.camclearance = (f32)data->slider.value;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%d", (s32)data->slider.value);
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerModCamMinDist(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = (s32)(g_ModOptions.cammindist + 0.5f);
		break;
	case MENUOP_SET:
		g_ModOptions.cammindist = (f32)data->slider.value;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%d", (s32)data->slider.value);
		break;
	}

	return 0;
}

/**
 * Camera Sideways, as a side and an amount rather than a signed number: the bar
 * has no minus sign on it to say which end is which, and "Left 80" needs no
 * explaining.
 */
static MenuItemHandlerResult menuhandlerModCamSide(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 side;

	switch (operation) {
	case MENUOP_GETSLIDER:
		side = (s32)(g_ModOptions.camside + (g_ModOptions.camside < 0 ? -0.5f : 0.5f));

		if (side < -MODCAM_MAXSIDE) {
			side = -MODCAM_MAXSIDE;
		} else if (side > MODCAM_MAXSIDE) {
			side = MODCAM_MAXSIDE;
		}

		data->slider.value = (side + MODCAM_MAXSIDE) / MODCAM_SIDESTEP;
		break;
	case MENUOP_SET:
		g_ModOptions.camside = (f32)((s32)data->slider.value * MODCAM_SIDESTEP - MODCAM_MAXSIDE);
		break;
	case MENUOP_GETSLIDERLABEL:
		side = (s32)data->slider.value * MODCAM_SIDESTEP - MODCAM_MAXSIDE;

		if (side < 0) {
			sprintf(data->slider.label, "Left %d", -side);
		} else if (side > 0) {
			sprintf(data->slider.label, "Right %d", side);
		} else {
			sprintf(data->slider.label, "Centre");
		}
		break;
	}

	return 0;
}

/**
 * Camera Forward/Back, read the same way as Camera Sideways: a direction and an
 * amount, since a bar with a minus sign on it says nothing about which end is
 * which. Forward is towards where the player is looking, and far enough forward
 * is round in front of them.
 */
static MenuItemHandlerResult menuhandlerModCamFwd(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 fwd;

	switch (operation) {
	case MENUOP_GETSLIDER:
		fwd = (s32)(g_ModOptions.camfwd + (g_ModOptions.camfwd < 0 ? -0.5f : 0.5f));

		if (fwd < -MODCAM_MAXFWD) {
			fwd = -MODCAM_MAXFWD;
		} else if (fwd > MODCAM_MAXFWD) {
			fwd = MODCAM_MAXFWD;
		}

		data->slider.value = (fwd + MODCAM_MAXFWD) / MODCAM_FWDSTEP;
		break;
	case MENUOP_SET:
		g_ModOptions.camfwd = (f32)((s32)data->slider.value * MODCAM_FWDSTEP - MODCAM_MAXFWD);
		break;
	case MENUOP_GETSLIDERLABEL:
		fwd = (s32)data->slider.value * MODCAM_FWDSTEP - MODCAM_MAXFWD;

		if (fwd < 0) {
			sprintf(data->slider.label, "Forward %d", -fwd);
		} else if (fwd > 0) {
			sprintf(data->slider.label, "Back %d", fwd);
		} else {
			sprintf(data->slider.label, "Centre");
		}
		break;
	}

	return 0;
}

/**
 * Camera Height, the third of the offsets and the last axis there is: straight
 * up in the world, so a raised camera looks down over the player without the
 * view having to be pitched down to find them.
 */
static MenuItemHandlerResult menuhandlerModCamHeight(s32 operation, struct menuitem *item, union handlerdata *data)
{
	s32 height;

	switch (operation) {
	case MENUOP_GETSLIDER:
		height = (s32)(g_ModOptions.camheight + (g_ModOptions.camheight < 0 ? -0.5f : 0.5f));

		if (height < -MODCAM_MAXHEIGHT) {
			height = -MODCAM_MAXHEIGHT;
		} else if (height > MODCAM_MAXHEIGHT) {
			height = MODCAM_MAXHEIGHT;
		}

		data->slider.value = (height + MODCAM_MAXHEIGHT) / MODCAM_HEIGHTSTEP;
		break;
	case MENUOP_SET:
		g_ModOptions.camheight = (f32)((s32)data->slider.value * MODCAM_HEIGHTSTEP - MODCAM_MAXHEIGHT);
		break;
	case MENUOP_GETSLIDERLABEL:
		height = (s32)data->slider.value * MODCAM_HEIGHTSTEP - MODCAM_MAXHEIGHT;

		if (height < 0) {
			sprintf(data->slider.label, "Down %d", -height);
		} else if (height > 0) {
			sprintf(data->slider.label, "Up %d", height);
		} else {
			sprintf(data->slider.label, "Centre");
		}
		break;
	}

	return 0;
}

/**
 * Camera Tether: the third person camera on a rod that pivots about the
 * player, and how tightly it is held behind the aim. See playerTetherCamera()
 * in player.c. Nothing to set with the camera on the eye, but the row is
 * shown either way, like the sliders above it.
 */
static MenuItemHandlerResult menuhandlerModCamTether(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Off", "Loose", "Normal", "Tight" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModOptions.camtether = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ModOptions.camtether;
	}

	return 0;
}

/**
 * Body Turn Speed: degrees per tick the tethered body turns to face where the
 * left stick sends it. Only read with Camera Tether on.
 */
static MenuItemHandlerResult menuhandlerModCamTurnSpeed(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = (g_ModOptions.camturnspeed - MODTURN_MIN) / MODTURN_STEP;
		break;
	case MENUOP_SET:
		g_ModOptions.camturnspeed = (s32)data->slider.value * MODTURN_STEP + MODTURN_MIN;
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%d", (s32)data->slider.value * MODTURN_STEP + MODTURN_MIN);
		break;
	}

	return 0;
}

/**
 * Spectator: the camera that comes off the player and flies.
 *
 * Start Spectating is the setting --spectate sets, and it holds for every stage
 * rather than for one match - which is what makes it the wrong control for
 * watching a single Combat Sim match. Spectator Start Game in the Combat Sim
 * menus is that one, and it does not touch this.
 *
 * The speed is units per frame at full stick, so the slider starts at 1 rather
 * than 0: a camera that does not move is what leaving the mode is for.
 */
#define MODSPECTATE_MINSPEED 1

static MenuItemHandlerResult menuhandlerModSpectateStart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return g_ModSpectateStart;
	case MENUOP_SET:
		g_ModSpectateStart = data->checkbox.value;
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerModSpectateSpeed(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETSLIDER:
		data->slider.value = (s32)(g_ModSpectateSpeed + 0.5f) - MODSPECTATE_MINSPEED;
		break;
	case MENUOP_SET:
		g_ModSpectateSpeed = (f32)(data->slider.value + MODSPECTATE_MINSPEED);
		break;
	case MENUOP_GETSLIDERLABEL:
		sprintf(data->slider.label, "%d", (s32)data->slider.value + MODSPECTATE_MINSPEED);
		break;
	}

	return 0;
}

/**
 * The same rows the Key Bindings page draws, for the binds that were taken off
 * it. The row says which bind it is in param3, rather than by where it sits in
 * the array the way menuhandlerBind() does, so the settings above can be
 * reordered without silently rebinding anything.
 *
 * One row per entry below - a bind with no row of its own can only be rebound
 * by editing pd.ini.
 */
static const struct menubind modMenuBinds[] = {
	{ CK_0400, "Spectator [+]\n", "N64 Ext 0400\n" },
	{ CK_1000, "Third Person [+]\n", "N64 Ext 1000\n" },
	{ CK_0800, "Combat Roll [+]\n",  "N64 Ext 0800\n" },
	// Akimbo Triggers: what it rebinds, so its layout can be changed here
	{ CK_0040,   "Fire Left [+]\n",       "N64 Ext 0040\n" },
	{ CK_ZTRIG,  "Fire Right [ZT]\n",     "N64 Z Trigger\n" },
	{ CK_0080,   "Fire Mode Left [+]\n",  "N64 Ext 0080\n" },
	{ CK_DPAD_R, "Fire Mode Right [DR]\n", "N64 D-Pad Right\n" },
	{ CK_RTRIG,  "Aim Mode [RT]\n",       "N64 R Trigger\n" },
	{ CK_DPAD_U, "Quick Menu [DU]\n",     "N64 D-Pad Up\n" },
};

static const char *menutextModBind(struct menuitem *item)
{
	return g_PlayerExtCfg[g_ExtMenuPlayer].extcontrols ?
		modMenuBinds[item->param3].name :
		modMenuBinds[item->param3].n64name;
}

static MenuItemHandlerResult menuhandlerModBind(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const s32 idx = item->param3;
	const u32 *binds;

	static char keyname[128];

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = INPUT_MAX_BINDS;
		break;
	case MENUOP_GETOPTIONTEXT:
		binds = inputKeyGetBinds(g_ExtMenuPlayer, modMenuBinds[idx].ck);
		if (binds && binds[data->dropdown.value]) {
			strncpy(keyname, inputGetKeyName(binds[data->dropdown.value]), sizeof(keyname) - 1);
			for (char *p = keyname; *p; ++p) {
				if (*p == '_') *p = ' ';
			}
			return (intptr_t)keyname;
		}
		return (intptr_t)"NONE";
	case MENUOP_SET:
		g_ExtendedBindKeyMenuItems[0].param2 = (uintptr_t)modMenuBinds[idx].name;
		g_BindIndex = data->dropdown.value;
		g_BindContKey = modMenuBinds[idx].ck;
		g_BindKeySetter = NULL;
		inputClearLastKey();
		menuPushDialog(&g_ExtendedBindKeyMenuDialog);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = 0;
	}

	return 0;
}

/**
 * Recording: the key, what it writes, and whether the red dot goes on screen.
 *
 * Frame Rate, Quality and Encoder get a row each. The preset and the container
 * do not and never will: the point of the feature is a file that plays, and a
 * menu that can produce one that does not is not doing anyone a favour. Encoder
 * is on the list because every option in it produces a file that plays - the
 * choice is what does the work and what it costs, not whether it works.
 * Mod.RecordEncoder in pd.ini names the ffmpeg to run, for anyone who keeps
 * theirs somewhere unusual.
 *
 * The frame rate is the recording's, not the game's. A game frame that arrives
 * early is not captured and one that arrives late is written twice, so the file
 * runs at the speed the match was watched at whatever the game managed.
 */
static const s32 g_ModRecordFpsValues[] = { 24, 30, 60 };

// libx264's CRF, low to high: lower is a bigger file and a better picture.
static const s32 g_ModRecordQualityValues[] = { 28, 24, 21, 17 };

/**
 * Turning recording on, and fetching an encoder if that is what it takes.
 *
 * Windows has no ffmpeg of its own and nothing is bundled, so the first time
 * this is switched on there is a download to agree to. Everywhere else the box
 * is just a box: recordEncoderIsMissing() is false and no dialog appears.
 */
static char g_RecordDownloadText[160];

static const char *menutextRecordDownloadSize(struct menuitem *item)
{
	snprintf(g_RecordDownloadText, sizeof(g_RecordDownloadText),
			"Download ffmpeg? About %dMB.\n", recordEncoderDownloadMb());

	return g_RecordDownloadText;
}

static char g_RecordDownloadDiskText[160];

static const char *menutextRecordDownloadDisk(struct menuitem *item)
{
	// Said out loud because the two numbers are not close: a shared build
	// compresses to well under half of what it occupies.
	snprintf(g_RecordDownloadDiskText, sizeof(g_RecordDownloadDiskText),
			"It unpacks to about %dMB in your game directory.\n",
			recordEncoderDownloadDiskMb());

	return g_RecordDownloadDiskText;
}

static MenuItemHandlerResult menuhandlerRecordDownloadConfirm(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		recordSetEnabled(1);
		recordFetchEncoder();
	}

	return 0;
}

struct menuitem g_ExtendedRecordDownloadMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)menutextRecordDownloadSize,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)menutextRecordDownloadDisk,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_SELECTABLE_CENTRE,
		L_OPTIONS_385, // "No"
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_SELECTABLE_CENTRE,
		L_OPTIONS_386, // "Yes"
		0,
		menuhandlerRecordDownloadConfirm,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedRecordDownloadMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Video Recording",
	g_ExtendedRecordDownloadMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerModRecordEnabled(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		// Not while one is running, and not while the encoder is on its way.
		return recordIsActive() || recordEncoderIsFetching();
	case MENUOP_GET:
		return recordIsEnabled();
	case MENUOP_SET:
		if (!data->checkbox.value) {
			recordSetEnabled(0);
			break;
		}

		if (recordEncoderIsMissing()) {
			// Left off until the download is agreed to, so a No leaves the row
			// where it was rather than on with nothing behind it.
			menuPushDialog(&g_ExtendedRecordDownloadMenuDialog);
			break;
		}

		recordSetEnabled(1);
		break;
	}

	return 0;
}

/**
 * What the row says while there is something to say.
 */
static char g_RecordEnabledText[96];

static const char *menutextModRecordEnabled(struct menuitem *item)
{
	if (recordEncoderIsFetching()) {
		snprintf(g_RecordEnabledText, sizeof(g_RecordEnabledText), "Video Recording - %s\n",
				recordEncoderFetchStatus());
	} else if (recordIsEnabled() && recordEncoderIsMissing()) {
		snprintf(g_RecordEnabledText, sizeof(g_RecordEnabledText),
				"Video Recording (no encoder)\n");
	} else {
		snprintf(g_RecordEnabledText, sizeof(g_RecordEnabledText), "Video Recording\n");
	}

	return g_RecordEnabledText;
}

static MenuItemHandlerResult menuhandlerModRecordFps(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "24", "30", "60" };

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return recordIsActive();
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		recordSetFps(g_ModRecordFpsValues[data->dropdown.value]);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = menuhandlerModPresetIndex(g_ModRecordFpsValues,
				ARRAYCOUNT(g_ModRecordFpsValues), recordGetFps());
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerModRecordQuality(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Small", "Medium", "Good", "Best" };
	s32 i;

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return recordIsActive();
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		recordSetQuality(g_ModRecordQualityValues[data->dropdown.value]);
		break;
	case MENUOP_GETSELECTEDINDEX:
		// The values run downwards, so the shared preset search does not fit.
		data->dropdown.value = 0;
		for (i = 0; i < (s32)ARRAYCOUNT(g_ModRecordQualityValues); i++) {
			if (recordGetQuality() <= g_ModRecordQualityValues[i]) {
				data->dropdown.value = i;
			}
		}
	}

	return 0;
}

/**
 * Which encoder does the work.
 *
 * Auto is the right answer nearly always: it tries each of this machine's in
 * turn and keeps the first that works, which is a few hundred milliseconds once
 * and never again. The row is here for the times it is not - a driver that
 * accepts the probe and then makes a mess of real frames, a second GPU that
 * should be doing this instead of the first, or wanting to see for yourself what
 * a different one costs.
 *
 * Software is libx264 across every core the machine has, which is the thing
 * recording on the GPU was for. It is on the list because a machine with nothing
 * on its GPU can still make a recording that way, not because anyone should
 * choose it otherwise.
 *
 * Most of the list cannot work on any one machine - an AMD card has no NVENC,
 * an Nvidia one has no VAAPI - so opening the dropdown starts the same probe a
 * recording would have run, and each name says whether it is any use here. It
 * is a thread and takes about a second, so the list says "checking" first and
 * fills in; nothing waits for it and a pick made meanwhile still works.
 *
 * A pick that turns out not to work is still not an error - the next recording
 * falls back to the search and writes what it found back here, so the row
 * corrects itself rather than quietly recording with something else. That is
 * the second line of defence now rather than the first.
 */
static MenuItemHandlerResult menuhandlerModRecordCodec(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return recordIsActive();
	case MENUOP_GETOPTIONCOUNT:
		// menuitemDropdownOverlay() asks for the count as it opens the list, so
		// this is the moment the player has said they want to choose - and the
		// last one before they are looking at names they could be misled by.
		recordProbeCodecs();
		data->dropdown.value = recordGetCodecCount();
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)recordGetCodecLabel(data->dropdown.value);
	case MENUOP_SET:
		recordSetCodecIndex(data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = recordGetCodecIndex();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerModRecordIndicator(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return recordGetIndicator();
	case MENUOP_SET:
		recordSetIndicator(data->checkbox.value);
		break;
	}

	return 0;
}

/**
 * Ghost Time Trial.
 *
 * The same setting the Ghost Trials page shows, kept here because this is where
 * every other thing the fork added is. It says what a trial does rather than
 * which missions record: nothing outside Ghost Trials records at all, so that
 * every run on a board was set under the same rules.
 *
 * Recording is on in both, because a run is only worth keeping once it has
 * turned out to be a good one, and by then it has been played.
 */
static MenuItemHandlerResult menuhandlerModGhost(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Record Only", "Record + Race" };

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModGhostMode = data->dropdown.value + MODGHOST_RECORD;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ModGhostMode <= MODGHOST_RECORD
			? 0 : g_ModGhostMode - MODGHOST_RECORD;
	}

	return 0;
}

/**
 * Whose ghost gets raced, out of the ones in the ghosts directory for this
 * stage and difficulty.
 *
 * Fastest is the leaderboard answer: drop somebody's run in the directory and
 * it is what you are chasing if it is better than yours. My Best is for the
 * days when that is not a useful thing to be chasing.
 */
static MenuItemHandlerResult menuhandlerModGhostPick(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Fastest Available", "My Best Only", "Chosen Ghosts" };

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return g_ModGhostMode != MODGHOST_RACE;
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModGhostPick = data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = g_ModGhostPick;
	}

	return 0;
}

static const s32 g_ModGhostAlphaValues[] = { 60, 110, 170, 230 };

static MenuItemHandlerResult menuhandlerModGhostAlpha(s32 operation, struct menuitem *item, union handlerdata *data)
{
	static const char *opts[] = { "Faint", "Normal", "Strong", "Solid" };

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return g_ModGhostMode != MODGHOST_RACE;
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = ARRAYCOUNT(opts);
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)opts[data->dropdown.value];
	case MENUOP_SET:
		g_ModGhostAlpha = g_ModGhostAlphaValues[data->dropdown.value];
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = menuhandlerModPresetIndex(g_ModGhostAlphaValues,
				ARRAYCOUNT(g_ModGhostAlphaValues), g_ModGhostAlpha);
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerModGhostSplits(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return g_ModGhostMode != MODGHOST_RACE;
	case MENUOP_GET:
		return g_ModGhostSplits;
	case MENUOP_SET:
		g_ModGhostSplits = data->checkbox.value;
		break;
	}

	return 0;
}

/**
 * The screenshot key. One key rather than the four slots a controller bind has,
 * and no N64 name, because there is no controller button behind it: it is read
 * straight off the keyboard so that a menu or a cutscene can be shot too.
 */
static const struct {
	const char *name;
	s32 (*get)(void);
	void (*set)(s32 vk);
} modKeyBinds[] = {
	{ "Screenshot\n",       screenshotGetKey, screenshotSetKey  },
	{ "Record Video\n",     recordGetKey,     recordSetKey      },
	{ "Dump Drawn Textures\n", texpackDumpGetKey, texpackDumpSetKey   },
	{ "Texture Packs On/Off\n", texpackToggleGetKey, texpackToggleSetKey },
	{ "Reload Packs\n",         texpackReloadGetKey, texpackReloadSetKey },
	{ "Next Texture Pack\n",    texpackCycleGetKey,  texpackCycleSetKey  },
	{ "XBLA Meshes On/Off\n",   xblaMeshToggleGetKey, xblaMeshToggleSetKey },
};

static const char *menutextModKeyBind(struct menuitem *item)
{
	return modKeyBinds[item->param3].name;
}

static MenuItemHandlerResult menuhandlerModKeyBind(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const s32 idx = item->param3;
	const s32 vk = modKeyBinds[idx].get();

	static char keyname[128];

	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = 1;
		break;
	case MENUOP_GETOPTIONTEXT:
		if (vk > 0) {
			strncpy(keyname, inputGetKeyName(vk), sizeof(keyname) - 1);
			keyname[sizeof(keyname) - 1] = '\0';
			for (char *p = keyname; *p; ++p) {
				if (*p == '_') *p = ' ';
			}
			return (intptr_t)keyname;
		}
		return (intptr_t)"NONE";
	case MENUOP_SET:
		g_ExtendedBindKeyMenuItems[0].param2 = (uintptr_t)modKeyBinds[idx].name;
		g_BindKeySetter = modKeyBinds[idx].set;
		inputClearLastKey();
		menuPushDialog(&g_ExtendedBindKeyMenuDialog);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = 0;
	}

	return 0;
}

/**
 * Dab's Mod Options is five pages that swipe left and right, the way the
 * Perfect Menu swipes to Options and the pause menu swipes to Inventory and
 * Mission Status: each page is a sibling dialog on the same layer
 * (menudialogdef.nextsibling), so a Left or Right that a row does not take
 * turns the page, and the chevrons beside the dialog show there is more.
 * The engine draws up to five siblings on a layer, which is why there are
 * five pages and not six; a sixth would be opened and never drawn.
 *
 * Every page has its own Back row, and a key bind sits on the page of the
 * feature it drives rather than on the Key Bindings page.
 *
 * The pages, in swipe order:
 *   Player    - what Jo can do: jump, roll, melee, akimbo, starting armed,
 *               and the fire/fire-mode binds.
 *   Camera    - tilt, COD aiming, the third-person camera, spectating.
 *   Display   - text, model LOD, texture enhancement, colours, and the
 *               texture-pack/XBLA mesh keys.
 *   Missions  - respawning, guards, the alarm, bodies, and Ghost Time Trials.
 *   Recording - video capture and the screenshot/record keys.
 */
struct menuitem g_ExtendedDabsModPlayerMenuItems[] = {
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Settings Preset",
		0,
		menuhandlerModPreset,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Jump",
		0,
		menuhandlerModJump,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Jump For",
		0,
		menuhandlerModJumpFor,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Combat Roll",
		0,
		menuhandlerModRoll,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		0,
		(uintptr_t)menutextModBind,
		2,
		menuhandlerModBind,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Melee Combos",
		0,
		menuhandlerModMelee,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Flinch When Shot",
		0,
		menuhandlerModFlinch,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Explosion Shake",
		0,
		menuhandlerModExplosionShake,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Tranquilizer Effect",
		0,
		menuhandlerModTranqEffect,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Start Armed",
		0,
		menuhandlerModStartArmed,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Start Armed For",
		0,
		menuhandlerModStartArmedFor,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Akimbo",
		0,
		menuhandlerModAkimbo,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Akimbo Triggers",
		0,
		menuhandlerModAkimboTriggers,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		0,
		(uintptr_t)menutextModBind,
		3,
		menuhandlerModBind,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		0,
		(uintptr_t)menutextModBind,
		4,
		menuhandlerModBind,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		0,
		(uintptr_t)menutextModBind,
		5,
		menuhandlerModBind,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		0,
		(uintptr_t)menutextModBind,
		6,
		menuhandlerModBind,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		0,
		(uintptr_t)menutextModBind,
		8,
		menuhandlerModBind,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menuitem g_ExtendedDabsModCameraMenuItems[] = {
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Camera Tilt",
		0,
		menuhandlerModCameraTilt,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Invert Camera Tilt",
		0,
		menuhandlerModTiltInvert,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Forward And Back Tilt",
		0,
		menuhandlerModTiltForward,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Gun Sway With Tilt",
		0,
		menuhandlerModGunSway,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"COD Style Aiming",
		0,
		menuhandlerModCodAiming,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Aim Lock",
		0,
		menuhandlerModCodAimLock,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		0,
		(uintptr_t)menutextModBind,
		7,
		menuhandlerModBind,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		0,
		(uintptr_t)menutextModBind,
		1,
		menuhandlerModBind,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Camera Preset",
		0,
		menuhandlerModCamPreset,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Camera Distance",
		540,
		menuhandlerModCamDist,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Camera Wall Clearance",
		120,
		menuhandlerModCamClearance,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Camera Minimum Distance",
		300,
		menuhandlerModCamMinDist,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Camera Sideways",
		2 * MODCAM_MAXSIDE / MODCAM_SIDESTEP,
		menuhandlerModCamSide,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Camera Forward/Back",
		2 * MODCAM_MAXFWD / MODCAM_FWDSTEP,
		menuhandlerModCamFwd,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Camera Height",
		2 * MODCAM_MAXHEIGHT / MODCAM_HEIGHTSTEP,
		menuhandlerModCamHeight,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Camera Tether",
		0,
		menuhandlerModCamTether,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Body Turn Speed",
		(MODTURN_MAX - MODTURN_MIN) / MODTURN_STEP,
		menuhandlerModCamTurnSpeed,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		0,
		(uintptr_t)menutextModBind,
		0,
		menuhandlerModBind,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Start Spectating",
		0,
		menuhandlerModSpectateStart,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Spectator Speed",
		199,
		menuhandlerModSpectateSpeed,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menuitem g_ExtendedDabsModDisplayMenuItems[] = {
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Thin Text Outlines",
		0,
		menuhandlerModCleanText,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Smooth Text",
		0,
		menuhandlerModSmoothText,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Model LOD",
		0,
		menuhandlerModModelLod,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Enhance Textures",
		0,
		menuhandlerModEnhanceTextures,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Stretched Edges",
		0,
		menuhandlerModStretchedEdges,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Vivid Colours",
		0,
		menuhandlerModVividColours,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Black Level",
		0,
		menuhandlerModBlackLevel,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		0,
		(uintptr_t)menutextModKeyBind,
		2,
		menuhandlerModKeyBind,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		0,
		(uintptr_t)menutextModKeyBind,
		3,
		menuhandlerModKeyBind,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		0,
		(uintptr_t)menutextModKeyBind,
		4,
		menuhandlerModKeyBind,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		0,
		(uintptr_t)menutextModKeyBind,
		5,
		menuhandlerModKeyBind,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		0,
		(uintptr_t)menutextModKeyBind,
		6,
		menuhandlerModKeyBind,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menuitem g_ExtendedDabsModMissionMenuItems[] = {
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Mission Respawn",
		0,
		menuhandlerModMissionRespawn,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Lives",
		0,
		menuhandlerModMissionLives,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Guards Alerted!",
		0,
		menuhandlerModGuardsAlerted,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Alerted Guards",
		0,
		menuhandlerModAlertedGuards,
	},
	{
		MENUITEMTYPE_SLIDER,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_SLIDER_WIDE,
		(uintptr_t)"Guard Spawn Speed",
		MODALARM_SPEED_MAX - MODALARM_SPEED_MIN,
		menuhandlerModGuardSpawnSpeed,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Guard Weapons",
		0,
		menuhandlerModGuardWeapons,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Alarm Sound",
		0,
		menuhandlerModAlarmSound,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Bodies",
		0,
		menuhandlerModBodies,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Body Time",
		0,
		menuhandlerModBodyTime,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Bodies Drawn",
		0,
		menuhandlerModBodiesDrawn,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Time Trial",
		0,
		menuhandlerModGhost,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Opponent",
		0,
		menuhandlerModGhostPick,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Visibility",
		0,
		menuhandlerModGhostAlpha,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Ghost Split Times",
		0,
		menuhandlerModGhostSplits,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menuitem g_ExtendedDabsModRecordingMenuItems[] = {
	{
		// A text function rather than a literal, so the row can say what the
		// download is doing while it happens.
		MENUITEMTYPE_CHECKBOX,
		0,
		0,
		(uintptr_t)menutextModRecordEnabled,
		0,
		menuhandlerModRecordEnabled,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Recording Frame Rate",
		0,
		menuhandlerModRecordFps,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Recording Quality",
		0,
		menuhandlerModRecordQuality,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Recording Encoder",
		0,
		menuhandlerModRecordCodec,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Recording Indicator",
		0,
		menuhandlerModRecordIndicator,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		0,
		(uintptr_t)menutextModKeyBind,
		1,
		menuhandlerModKeyBind,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		0,
		(uintptr_t)menutextModKeyBind,
		0,
		menuhandlerModKeyBind,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

// The chain is declared last-to-first so each page can name the next.
struct menudialogdef g_ExtendedDabsModRecordingMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Mods: Recording",
	g_ExtendedDabsModRecordingMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

struct menudialogdef g_ExtendedDabsModMissionMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Mods: Missions",
	g_ExtendedDabsModMissionMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	&g_ExtendedDabsModRecordingMenuDialog,
};

struct menudialogdef g_ExtendedDabsModDisplayMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Mods: Display",
	g_ExtendedDabsModDisplayMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	&g_ExtendedDabsModMissionMenuDialog,
};

struct menudialogdef g_ExtendedDabsModCameraMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Mods: Camera",
	g_ExtendedDabsModCameraMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	&g_ExtendedDabsModDisplayMenuDialog,
};

// The head of the chain, and the one Extended Options opens.
struct menudialogdef g_ExtendedDabsModMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Mods: Player",
	g_ExtendedDabsModPlayerMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	&g_ExtendedDabsModCameraMenuDialog,
};

/**
 * Texture packs.
 *
 * The list is re-read whenever the dropdown is opened rather than at startup,
 * so a pack dropped into texture-packs/ while the game is running turns up
 * without a restart - which is most of the point of the reload key.
 */
static MenuItemHandlerResult menuhandlerTexturePack(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		texpackRefreshPacks();
		data->dropdown.value = texpackGetNumPacks() + 1; // plus "None"
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)(data->dropdown.value == 0
				? "None" : texpackGetPackName(data->dropdown.value - 1));
	case MENUOP_SET:
		texpackSetSelectedPack((s32)data->dropdown.value - 1);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = texpackGetSelectedPack() + 1;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerTexturePackEnabled(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return texpackLoadEnabled();
	case MENUOP_SET:
		texpackSetLoadEnabled(data->checkbox.value);
		break;
	}

	return 0;
}

/**
 * Model packs, the same shape as the texture packs above them: the list is
 * re-read whenever the dropdown opens, so a folder dropped into model-packs/
 * while the game is running turns up without a restart.
 */
static MenuItemHandlerResult menuhandlerModelPack(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GETOPTIONCOUNT:
		modelpackRefreshPacks();
		data->dropdown.value = modelpackGetNumPacks() + 1; // plus "None"
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)(data->dropdown.value == 0
				? "None" : modelpackGetPackName(data->dropdown.value - 1));
	case MENUOP_SET:
		modelpackSetSelectedPack((s32)data->dropdown.value - 1);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = modelpackGetSelectedPack() + 1;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerModelPackEnabled(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return modelpackLoadEnabled();
	case MENUOP_SET:
		modelpackSetLoadEnabled(data->checkbox.value);
		break;
	}

	return 0;
}

/**
 * Which of the two a model that has both draws: the pack's file for the
 * game's own model, or the XBLA release's mesh for it (or the pack's own
 * replacement for that mesh, where it ships one).
 *
 * Only ever visible with the release's meshes on, because with them off there
 * is no second thing for a pack's model to lose to. Live either way: nothing
 * is built for this, the draw reads it.
 */
static MenuItemHandlerResult menuhandlerModelPackPrefer(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKHIDDEN:
		return !xblaMeshIsAvailable() || !xblaMeshGetEnabled();
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = 2;
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)(data->dropdown.value == MODELPACK_PREFER_XBLA
				? "XBLA Mesh" : "Pack's Model");
	case MENUOP_SET:
		modelpackSetPrefer((s32)data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = modelpackGetPrefer();
	}

	return 0;
}

/**
 * The asset dump: every texture and model, the XBLA release's included when
 * there is one, in one go. One row that starts it and stops it, and a line
 * under it saying where it is up to, which stays once it is done.
 */
static MenuItemHandlerResult menuhandlerAssetDump(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		if (assetDumpIsRunning()) {
			assetDumpCancel();
		} else {
			assetDumpStart();
		}
	}

	return 0;
}

static const char *menutextAssetDump(struct menuitem *item)
{
	return assetDumpIsRunning() ? "Stop Dumping\n" : "Dump All Assets To Disk\n";
}

static const char *menutextAssetDumpStatus(struct menuitem *item)
{
	static char text[160];

	snprintf(text, sizeof(text), "%s\n", assetDumpGetStatus());

	return text;
}

static MenuItemHandlerResult menuhandlerAssetDumpStatus(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return assetDumpGetStatus()[0] == '\0';
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerTexturePackReload(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		texpackReload();
		modelpackReload();
	}

	return 0;
}

/**
 * Deleting a pack, having asked. A pack is a folder of a few thousand files and
 * there is no undo, so the name goes in the question.
 */
static char g_TexturePackDeleteText[96];

static const char *menutextTexturePackDeleteAsk(struct menuitem *item)
{
	snprintf(g_TexturePackDeleteText, sizeof(g_TexturePackDeleteText), "Delete %s?\n",
			texpackGetPackName(texpackGetSelectedPack()));

	return g_TexturePackDeleteText;
}

static MenuItemHandlerResult menuhandlerTexturePackDeleteConfirm(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_SET) {
		texpackDeletePack(texpackGetSelectedPack());
	}

	return 0;
}

struct menuitem g_ExtendedTexturePackDeleteMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)menutextTexturePackDeleteAsk,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)"Its files are removed from disk.\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_SELECTABLE_CENTRE,
		L_OPTIONS_385, // "No"
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG | MENUITEMFLAG_SELECTABLE_CENTRE,
		L_OPTIONS_386, // "Yes"
		0,
		menuhandlerTexturePackDeleteConfirm,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedTexturePackDeleteMenuDialog = {
	MENUDIALOGTYPE_DANGER,
	(uintptr_t)"Delete Texture Pack",
	g_ExtendedTexturePackDeleteMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static MenuItemHandlerResult menuhandlerTexturePackDelete(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		// Nothing selected is nothing to delete.
		return texpackGetSelectedPack() < 0;
	case MENUOP_SET:
		menuPushDialog(&g_ExtendedTexturePackDeleteMenuDialog);
		break;
	}

	return 0;
}

/**
 * The reload row, which doubles as where the pack's coverage is reported.
 *
 * A text function, which is what param2 is when MENUITEMFLAG_LITERAL_TEXT is
 * absent - see menuResolveParam2Text(). Setting that flag on a function pointer
 * hands the menu a "string" that is really machine code, which is a crash on
 * the first draw rather than a wrong label.
 *
 * The count rides on this item rather than a MENUITEMTYPE_LABEL of its own
 * because a label following a selectable is laid out on the same row as it and
 * draws over the top.
 */
static char g_TexturePackCountText[64];

static const char *menutextTexturePackReload(struct menuitem *item)
{
	const s32 count = texpackGetNumReplacements();

	if (!texpackLoadEnabled()) {
		snprintf(g_TexturePackCountText, sizeof(g_TexturePackCountText),
				"Reload Pack (turned off)\n");
	} else if (count > 0) {
		const s32 unplaced = texpackGetNumUnplaced();

		if (unplaced) {
			// The second pair is the pack's model textures, which have no
			// texture number and are only recognised once something draws
			// them - so it climbs as you play rather than being known up front.
			snprintf(g_TexturePackCountText, sizeof(g_TexturePackCountText),
					"Reload Pack (%d + %d of %d)\n", count,
					texpackGetNumTexelMatched(), unplaced);
		} else {
			snprintf(g_TexturePackCountText, sizeof(g_TexturePackCountText),
					"Reload Pack (%d replaced)\n", count);
		}
	} else if (texpackGetNumUnplaced()) {
		// A pack can be nothing but model textures, which have no texture
		// number - so none are placed up front and "none found" would be
		// wrong about a pack that works.
		snprintf(g_TexturePackCountText, sizeof(g_TexturePackCountText),
				"Reload Pack (%d of %d by texels)\n",
				texpackGetNumTexelMatched(), texpackGetNumUnplaced());
	} else {
		snprintf(g_TexturePackCountText, sizeof(g_TexturePackCountText),
				"Reload Pack (none found)\n");
	}

	return g_TexturePackCountText;
}

/**
 * The two halves of the release, switched on separately.
 *
 * They are separate because either one on its own is a thing somebody wants:
 * the meshes are the release's geometry and the textures are its art, and an
 * untextured mesh is a flat pale solid that says whether a shape is right
 * without an art problem on top of it. A texture pack goes on underneath both
 * - the meshes' own materials name records past the ones that carry a texture
 * number, so a pack cannot reach them and never has to be turned off to see
 * them.
 *
 * "Enable Textures" is the release's art everywhere, not only on its meshes:
 * a texture that carries a number is served the release's record for that
 * number as the renderer asks for it, which is the same picture the button
 * below would have written into a pack and saves converting one at all
 * (xblaTexLoadNumbered()). A pack the player selected still wins, texture by
 * texture, so this fills in what the pack has no file for.
 *
 * Both are live in a level: models are matched against the release's copy as
 * they load whether or not the switch is on, and a texture is decided as the
 * picture is handed to the renderer rather than as a display list is built.
 * Neither needs a note under it saying when it applies, except in the one case
 * where a model load cannot have done the matching - see menutextXblaMeshLate().
 */
static MenuItemHandlerResult menuhandlerXblaMeshes(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return xblaMeshGetEnabled();
	case MENUOP_SET:
		xblaMeshSetEnabled(!xblaMeshGetEnabled());
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerXblaMeshTextures(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return xblaTexGetEnabled();
	case MENUOP_SET:
		xblaTexSetEnabled(!xblaTexGetEnabled());
		break;
	}

	return 0;
}

/**
 * The release's own logo, as the page's banner.
 *
 * Drawn the way the Community Packs page draws a pack's cover (see
 * menuhandlerCommunityPoster()): a label row whose text is nothing but the
 * space it reserves, and a custom render that puts the picture in it. The
 * picture is a Textures.raw record rather than a PNG in the binary - xblaui.c
 * - and the row is not there at all for a player with no package, since the
 * whole page is inert for them anyway.
 */
#define XBLA_LOGOTEXT "\n\n\n\n"

static MenuItemHandlerResult menuhandlerXblaLogo(s32 operation, struct menuitem *item, union handlerdata *data)
{
	struct menudialog *dialog = g_Menus[g_MpPlayerNum].curdialog;
	struct menuitemrenderdata *renderdata;
	struct menuimage *img = xblaUiGetLogo();
	Gfx *gdl;
	s32 textheight;
	s32 textwidth;
	s32 width;
	s32 height;
	s32 x1;
	s32 y1;

	if (operation == MENUOP_CHECKHIDDEN) {
		return img == NULL;
	}

	if (operation != MENUOP_RENDER) {
		return 0;
	}

	gdl = data->type19.gdl;
	renderdata = data->type19.renderdata2;

	if (!dialog || !img) {
		return (intptr_t)gdl;
	}

	textMeasure(&textheight, &textwidth, (char *)item->param2,
			g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);

	height = textheight - 2;
	width = XBLAUI_LOGO_ASPECT(height);

	x1 = dialog->x + (dialog->width - width) / 2;
	y1 = renderdata->y + 1;

	return (intptr_t)menuImageDraw(gdl, img, x1, y1, x1 + width, y1 + height, 255);
}

/**
 * "Enable Font": the release's own glyphs on the game's own text.
 *
 * Separate from "Enable Textures" because the release's text art is not one of
 * its textures - a glyph has no texture number and never went through the
 * pack - and because it is the one part of the release's art a player might
 * want the ROM's version of while taking the rest: the typeface is the same,
 * but the release set it from the outline where the ROM has a bitmap, so the
 * letters are sharper and a little lighter. Live like the other two: the
 * picture is fitted into the ROM's own cell as it is handed to the renderer,
 * so nothing reflows and nothing is rebuilt.
 */
static MenuItemHandlerResult menuhandlerXblaFont(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return xblaFontGetEnabled();
	case MENUOP_SET:
		xblaFontSetEnabled(!xblaFontGetEnabled());
		break;
	}

	return 0;
}

/**
 * "Enable Explosions": the release's own 48 frame fireball.
 *
 * Its own switch rather than part of "Enable Textures", because it is not a
 * texture: 4J left the ROM's explosion records alone and drew their own
 * animation somewhere the ROM has no number for, so this is a picture put
 * where the game did not ask for one. Everything about how an explosion is
 * drawn is still the game's - see xblaexpl.h.
 */
static MenuItemHandlerResult menuhandlerXblaExplosions(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return xblaExplGetEnabled();
	case MENUOP_SET:
		xblaExplSetEnabled(!xblaExplGetEnabled());
		break;
	}

	return 0;
}

static char g_XblaMeshPackText[80];

/**
 * Said only when a pack has pictures of its own for the meshes' textures.
 *
 * The release's own art is the whole point of the checkbox above, so a player
 * who has repainted some of it is the one person on this page who cannot tell
 * from the picture whether their folder was read at all - the records have no
 * texture number, so the count on the Texture Packs page does not cover them.
 */
static const char *menutextXblaMeshPack(struct menuitem *item)
{
	snprintf(g_XblaMeshPackText, sizeof(g_XblaMeshPackText),
			"%d of them replaced from the texture pack\n", texpackGetNumXblaReplacements());

	return g_XblaMeshPackText;
}

static MenuItemHandlerResult menuhandlerXblaMeshPack(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return texpackGetNumXblaReplacements() == 0;
	}

	return 0;
}

/**
 * The rooms follow the same rule as the pictures - the release's geometry
 * is the models feature applied to the levels, so this counts while the
 * meshes are on - and it is as live as they are: the rooms loaded under the
 * old setting are dropped and come back from the other copy (xblastage.h).
 */
static MenuItemHandlerResult menuhandlerXblaStages(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_GET:
		return xblaStageGetEnabled();
	case MENUOP_SET:
		xblaStageSetEnabled(!xblaStageGetEnabled());
		break;
	}

	return 0;
}

static char g_XblaMeshLateText[64];

/**
 * The one time a checkbox on this page does nothing that can be seen, said
 * only while it is true.
 *
 * A player whose copy is still inside its .7z matches no model as a level
 * loads, because a model load takes the package only if it is ready and never
 * unpacks 250MB on somebody who may only want the textures. Their first
 * switching of the meshes on is what takes the archive apart, and it is over
 * by the time this draws - but the level behind the menu was loaded before
 * there was anything to match it against, so it stays stock and the next one
 * does not.
 *
 * This is a fact xblamesh.c holds rather than a guess at whether the switch
 * applied, which is what the note that used to be here got wrong: it is set
 * when that unpack happens in a level and cleared at the next lvReset(), so
 * every other player and every later level never see it.
 */
static const char *menutextXblaMeshLate(struct menuitem *item)
{
	snprintf(g_XblaMeshLateText, sizeof(g_XblaMeshLateText),
			"Unpacked now - models from the next level\n");

	return g_XblaMeshLateText;
}

/**
 * The row is not there at all the rest of the time.
 *
 * A label that draws a blank line still takes one, and every player who has
 * ever had a package on disk would be paying that gap for a sentence they are
 * never going to read - which is half of what was wrong with the note that
 * used to sit here.
 */
static MenuItemHandlerResult menuhandlerXblaMeshLate(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKHIDDEN) {
		return !xblaMeshModelsAreLate();
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerXblaStart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const s32 state = xblaImportGetState();

	if (operation != MENUOP_SET) {
		return 0;
	}

	if (state == XBLAIMPORT_EXTRACTING || state == XBLAIMPORT_READING ||
			state == XBLAIMPORT_CONVERTING) {
		xblaImportCancel();
	} else {
		xblaImportStart();
	}

	return 0;
}

static char g_XblaStartText[80];

static const char *menutextXblaStart(struct menuitem *item)
{
	const s32 state = xblaImportGetState();

	if (state == XBLAIMPORT_EXTRACTING || state == XBLAIMPORT_READING ||
			state == XBLAIMPORT_CONVERTING) {
		snprintf(g_XblaStartText, sizeof(g_XblaStartText), "Cancel\n");
	} else {
		snprintf(g_XblaStartText, sizeof(g_XblaStartText), "Make Into Texture Pack\n");
	}

	return g_XblaStartText;
}

static char g_XblaStatusText[160];

static const char *menutextXblaStatus(struct menuitem *item)
{
	const s32 state = xblaImportGetState();

	if (!xblaImportIsAvailable()) {
		snprintf(g_XblaStatusText, sizeof(g_XblaStatusText),
				"No package found - put Perfect Dark XBLA.7z in the xbla folder\n");
	} else if (state == XBLAIMPORT_IDLE) {
		snprintf(g_XblaStatusText, sizeof(g_XblaStatusText),
				"Ready - this takes about a minute\n");
	} else if (state == XBLAIMPORT_CONVERTING) {
		snprintf(g_XblaStatusText, sizeof(g_XblaStatusText), "%s  %d%%\n",
				xblaImportGetStatus(), xblaImportGetPercent());
	} else {
		snprintf(g_XblaStatusText, sizeof(g_XblaStatusText), "%s\n",
				xblaImportGetStatus());
	}

	return g_XblaStatusText;
}

// Only the file name is shown, not the whole path, and only this much of it.
#define XBLA_PATHCHARS 42
static char g_XblaPathText[128];

static const char *menutextXblaPath(struct menuitem *item)
{
	const char *path = xblaImportGetPackagePath();
	const char *slash;

	if (!path[0]) {
		// Nothing found: the folder to drop it in is the useful thing to show,
		// and its tail is the part a player can act on.
		const char *dir = xblaImportGetDropDir();
		const u32 len = strlen(dir);

		if (len > XBLA_PATHCHARS) {
			snprintf(g_XblaPathText, sizeof(g_XblaPathText), "In ...%s\n",
					dir + len - (XBLA_PATHCHARS - 3));
		} else {
			snprintf(g_XblaPathText, sizeof(g_XblaPathText), "In %s\n", dir);
		}

		return g_XblaPathText;
	}

	// The whole path does not fit and the leading directories are not the
	// interesting part; the file name says which copy was found.
	slash = strrchr(path, '/');
#ifdef PLATFORM_WIN32
	if (!slash) {
		slash = strrchr(path, '\\');
	}
#endif

	path = slash ? slash + 1 : path;

	// A content id is 42 characters and already fills the row, so a longer
	// name is cut rather than let run into the panel edge.
	if (strlen(path) > XBLA_PATHCHARS) {
		snprintf(g_XblaPathText, sizeof(g_XblaPathText), "From %.*s...\n",
				XBLA_PATHCHARS - 3, path);
	} else {
		snprintf(g_XblaPathText, sizeof(g_XblaPathText), "From %s\n", path);
	}

	return g_XblaPathText;
}

struct menuitem g_ExtendedXblaMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LIST_CUSTOMRENDER,
		(uintptr_t)XBLA_LOGOTEXT,
		0,
		menuhandlerXblaLogo,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Enable Models/Meshes",
		0,
		menuhandlerXblaMeshes,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Enable Textures",
		0,
		menuhandlerXblaMeshTextures,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)menutextXblaMeshPack,
		0,
		menuhandlerXblaMeshPack,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Enable Level Geometry",
		0,
		menuhandlerXblaStages,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Enable Font",
		0,
		menuhandlerXblaFont,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Enable Explosions",
		0,
		menuhandlerXblaExplosions,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)menutextXblaMeshLate,
		0,
		menuhandlerXblaMeshLate,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)menutextXblaPath,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		0,
		(uintptr_t)menutextXblaStart,
		0,
		menuhandlerXblaStart,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)menutextXblaStatus,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedXblaMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Xbox 360 (XBLA)",
	g_ExtendedXblaMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

struct menuitem g_ExtendedTexturePackMenuItems[] = {
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Texture Pack",
		0,
		menuhandlerTexturePack,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Use Texture Packs",
		0,
		menuhandlerTexturePackEnabled,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		0,
		(uintptr_t)menutextTexturePackReload,
		0,
		menuhandlerTexturePackReload,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Delete Pack...\n",
		0,
		menuhandlerTexturePackDelete,
	},
	{
		// Packs other people have made, fetched from their own release pages.
		// Above the separator with the pack list rather than below it with the
		// tools, because installing one is the same act as choosing one.
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Community Packs...\n",
		0,
		(void *)&g_CommunityMenuDialog,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Model Pack",
		0,
		menuhandlerModelPack,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Use Model Packs",
		0,
		menuhandlerModelPackEnabled,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"A Model With Both Draws",
		0,
		menuhandlerModelPackPrefer,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		0,
		(uintptr_t)menutextAssetDump,
		0,
		menuhandlerAssetDump,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)menutextAssetDumpStatus,
		0,
		menuhandlerAssetDumpStatus,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Xbox 360 (XBLA)\n",
		0,
		(void *)&g_ExtendedXblaMenuDialog,
	},

	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedTexturePackMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Texture & Model Packs",
	g_ExtendedTexturePackMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * Load Mods.
 *
 * Choosing a mod swaps it where we stand, the same as the texture pack page
 * above: nothing outside romdata holds file data, so dropping the file slots
 * and reading them again is enough. A mod with a segs/ directory is the one
 * exception and only records what the next start should mount - segments are
 * read once into MEMPOOL_PERMANENT, which is never given back, and the game
 * holds raw pointers into all of it. modListSwap() returns false for those,
 * and the Restart Now item below is what finishes the job.
 *
 * The list is re-read whenever the dropdown opens, the same as texture packs,
 * so a mod folder dropped in while the game is running turns up without
 * needing this start to have known about it.
 */
static MenuItemHandlerResult menuhandlerModDir(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return modListIsFromArgs();
	case MENUOP_GETOPTIONCOUNT:
		modListRefresh();
		data->dropdown.value = modListGetCount() + 1; // plus "None"
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)(data->dropdown.value == 0
				? "None" : modListGetName(data->dropdown.value - 1));
	case MENUOP_SET:
		{
			const s32 index = (s32)data->dropdown.value - 1;

			// Swapped where we stand when the mod allows it; otherwise this is
			// only a note of what the next start should mount.
			if (!modListSwap(index)) {
				modListSetSelected(index);
			}
		}
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = modListGetSelected() + 1;
	}

	return 0;
}

/**
 * Whether the selection and what is loaded are the same thing. The name is what
 * is compared rather than the list index, because the mod that is loaded may
 * have been mounted from the command line and never be in the list at all.
 */
static bool menuModDirPending(void)
{
	const char *loaded = modListGetLoadedName();
	const char *selected = modListGetSelectedName();

	if (modListIsFromArgs()) {
		return false;
	}

	if (!loaded) {
		return selected[0] != '\0';
	}

	return strcmp(loaded, selected) != 0;
}

static char g_ModDirStatusText[160];

static const char *menutextModDirStatus(struct menuitem *item)
{
	const char *loaded = modListGetLoadedName();
	const char *selected = modListGetSelectedName();

	if (modListIsFromArgs()) {
		snprintf(g_ModDirStatusText, sizeof(g_ModDirStatusText),
				"Loaded from the command line: %s\n", loaded ? loaded : "none");
	} else if (menuModDirPending()) {
		// Only a mod that replaces ROM segments gets this far; anything else
		// was swapped in when it was chosen.
		if (selected[0]) {
			snprintf(g_ModDirStatusText, sizeof(g_ModDirStatusText),
					"%s replaces ROM segments. Restart to load it.\n", selected);
		} else {
			snprintf(g_ModDirStatusText, sizeof(g_ModDirStatusText),
					"Restart to play without a mod.\n");
		}
	} else if (loaded) {
		snprintf(g_ModDirStatusText, sizeof(g_ModDirStatusText), "Loaded: %s\n", loaded);
	} else {
		snprintf(g_ModDirStatusText, sizeof(g_ModDirStatusText), "No mod is loaded.\n");
	}

	return g_ModDirStatusText;
}

static MenuItemHandlerResult menuhandlerModDirRestart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return !menuModDirPending();
	}

	if (operation == MENUOP_SET) {
		// The way out is the ordinary one - the same exit() Exit Game calls -
		// so that the config naming the mod, the binds and an unfinished
		// recording are all written before anything starts again. cleanup()
		// does the starting, once there is nothing left open to share.
		sysRequestRestart();
		exit(0);
	}

	return 0;
}

struct menuitem g_ExtendedModsMenuItems[] = {
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Mod",
		0,
		menuhandlerModDir,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)menutextModDirStatus,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Restart Now\n",
		0,
		menuhandlerModDirRestart,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)"One mod at a time. Mods live in mods/, or beside\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)"the game in a folder named mod-something. One that\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)"replaces ROM audio or textures needs a restart.\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedModsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Load Mods",
	g_ExtendedModsMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

/**
 * Stage Loader.
 *
 * Every installed mod's maps as extra Combat Simulator arenas, beside whatever
 * mod is loaded. A mod chosen here is mounted for its maps alone
 * (fsAddMapsDir): the mod loader pins its bg, pads and setup files to it and
 * gives each map a stage number of its own, so nothing it ships replaces a
 * stock file or a stock map. Turning a mod on or off swaps where we stand
 * through the same path Load Mods uses; a loaded mod that replaces ROM
 * segments cannot be swapped, and then Restart Now finishes it.
 */
static s32 g_MapsMenuMod = -1;   // the mod the dropdown shows

static MenuItemHandlerResult menuhandlerMapsAll(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return modListIsFromArgs();
	case MENUOP_GET:
		return modMapsAllEnabled();
	case MENUOP_SET:
		modMapsSetAll(data->checkbox.value);
		modMapsApply();
		break;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMapsMod(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return modListIsFromArgs();
	case MENUOP_GETOPTIONCOUNT:
		modListRefresh();
		data->dropdown.value = modListGetCount();
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)modListGetName(data->dropdown.value);
	case MENUOP_SET:
		g_MapsMenuMod = (s32)data->dropdown.value;
		break;
	case MENUOP_GETSELECTEDINDEX:
		if (g_MapsMenuMod < 0 || g_MapsMenuMod >= modListGetCount()) {
			g_MapsMenuMod = modListGetCount() > 0 ? 0 : -1;
		}
		data->dropdown.value = g_MapsMenuMod < 0 ? 0 : g_MapsMenuMod;
	}

	return 0;
}

static MenuItemHandlerResult menuhandlerMapsModEnabled(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const char *name = g_MapsMenuMod >= 0 ? modListGetName(g_MapsMenuMod) : NULL;

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		return modListIsFromArgs() || !name || modMapsAllEnabled();
	case MENUOP_GET:
		return name ? modMapsIsEnabled(name) : 0;
	case MENUOP_SET:
		if (name) {
			modMapsSetEnabled(name, data->checkbox.value);
			modMapsApply();
		}
		break;
	}

	return 0;
}

static char g_MapsStatusText[160];

static const char *menutextMapsStatus(struct menuitem *item)
{
	s32 registered, found, mods;

	modloaderGetStats(&registered, &found, &mods);

	if (modListIsFromArgs()) {
		snprintf(g_MapsStatusText, sizeof(g_MapsStatusText), "Mods came from the command line.\n");
	} else if (modMapsPending()) {
		snprintf(g_MapsStatusText, sizeof(g_MapsStatusText), "The loaded mod replaces ROM segments. Restart to apply.\n");
	} else if (registered < found) {
		snprintf(g_MapsStatusText, sizeof(g_MapsStatusText), "%d of %d maps from %d mod%s: out of stage numbers.\n",
				registered, found, mods, mods == 1 ? "" : "s");
	} else if (registered) {
		snprintf(g_MapsStatusText, sizeof(g_MapsStatusText), "%d map%s from %d mod%s in the Combat Simulator.\n",
				registered, registered == 1 ? "" : "s", mods, mods == 1 ? "" : "s");
	} else {
		snprintf(g_MapsStatusText, sizeof(g_MapsStatusText), "No mod maps loaded.\n");
	}

	return g_MapsStatusText;
}

static MenuItemHandlerResult menuhandlerMapsRestart(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return !modMapsPending();
	}

	if (operation == MENUOP_SET) {
		sysRequestRestart();
		exit(0);
	}

	return 0;
}

struct menuitem g_ExtendedMapsMenuItems[] = {
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Use maps from every installed mod\n",
		0,
		menuhandlerMapsAll,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Mod",
		0,
		menuhandlerMapsMod,
	},
	{
		MENUITEMTYPE_CHECKBOX,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Use this mod's maps\n",
		0,
		menuhandlerMapsModEnabled,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)menutextMapsStatus,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Restart Now\n",
		0,
		menuhandlerMapsRestart,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)"A mod's maps join the Combat Simulator's arena list\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)"beside the game's own, named after the map and the\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING,
		(uintptr_t)"mod. Nothing in the game's own maps is replaced.\n",
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedMapsMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Stage Loader",
	g_ExtendedMapsMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

struct menuitem g_ExtendedMenuItems[] = {
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Video\n",
		0,
		(void *)&g_ExtendedVideoMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Audio\n",
		0,
		(void *)&g_ExtendedAudioMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Mouse\n",
		0,
		(void *)&g_ExtendedMouseMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Controller\n",
		0,
		menuhandlerOpenControllerMenu,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Game\n",
		0,
		menuhandlerOpenGameMenu,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Key Bindings\n",
		0,
		menuhandlerOpenBindsMenu,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Dab's Mod Options\n",
		0,
		(void *)&g_ExtendedDabsModMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Texture & Model Packs\n",
		0,
		(void *)&g_ExtendedTexturePackMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Load Mods\n",
		0,
		(void *)&g_ExtendedModsMenuDialog,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_OPENSDIALOG | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Stage Loader\n",
		0,
		(void *)&g_ExtendedMapsMenuDialog,
	},
	{
		MENUITEMTYPE_SEPARATOR,
		0,
		0,
		0,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG,
		L_OPTIONS_213, // "Back"
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_ExtendedMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Extended Options",
	g_ExtendedMenuItems,
	NULL,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

void updateMaxAnisotropyLevel()
{
	for (int i = 0; i < ARRAYCOUNT(g_ExtendedVideoMenuItems); ++i) {
		struct menuitem *item = &g_ExtendedVideoMenuItems[i];
		const char *text = menuResolveParam2Text(item);
		
		if (text && strstr(text, "Anisotropic Filtering") != NULL) {
			item->param3 = videoGetMaxAnisotropyLevel();
			break;
		}
	}

}

void optionsMenuInit()
{
	updateMaxAnisotropyLevel();
}
