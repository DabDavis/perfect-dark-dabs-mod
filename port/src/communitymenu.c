#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "data.h"
#include "types.h"
#include "game/mainmenu.h"
#include "game/menu.h"
#include "bss.h"
#include "game/menugfx.h"
#include "game/game_1531a0.h"
#include "lib/vi.h"
#include "texpack.h"
#include "menuimage.h"
#include "community.h"

/**
 * Community Packs, under Extended Options -> Texture Packs.
 *
 * The page is one pack at a time: what it is, who made it, what it looks like,
 * and one row that says what pressing it does now - ask, download, or nothing
 * because it is already installed. The work is all in community.c and all on a
 * worker thread; this polls, the way the update page polls updateGetState().
 *
 * The cover art is the reason this is a file rather than another page in
 * optionsmenu.c. A menu item cannot draw a picture - everything the menus put
 * on screen is a texture number and a pack's cover art is not one - so it goes
 * through menuimage.c, and the row it goes in is a label of blank lines whose
 * only job is to reserve the space for it (MENUITEMFLAG_LIST_CUSTOMRENDER,
 * which hands a menu item's own rectangle to a handler that draws).
 */

// The blank lines the poster is drawn over. How tall that makes the row is not
// this file's arithmetic to do - a label's height is what textMeasure() says
// about its text plus the menu's own padding - so the picture is measured from
// the same string at the moment it is drawn. Ten lines is what the page can
// spare without scrolling.
#define COMMUNITY_POSTERTEXT "\n\n\n\n\n\n\n\n\n\n"

// Two thirds, which is the shape of the art a pack is announced with.
#define COMMUNITY_POSTERW(h) ((h) * 2 / 3)

static char g_CommunityText[256];

static const char *menutextCommunityPack(struct menuitem *item)
{
	const s32 index = communityGetSelected();

	snprintf(g_CommunityText, sizeof(g_CommunityText), "%s\nby %s\n%s",
			communityGetName(index), communityGetAuthor(index), communityGetBlurb(index));

	return g_CommunityText;
}

/**
 * Where the pack came from, so that anybody who would rather do this by hand -
 * or wants to read what they are about to download - has the address.
 */
static const char *menutextCommunitySource(struct menuitem *item)
{
	static char text[192];

	const char *url = communityGetSource(communityGetSelected());
	const char *slash = strrchr(url, '/');

	// Over two lines at the last slash: a repository path is longer than the
	// window is wide and what gets cut off is the half that names the pack.
	if (slash) {
		snprintf(text, sizeof(text), "%.*s\n%s\n", (s32)(slash - url + 1), url, slash + 1);
	} else {
		snprintf(text, sizeof(text), "%s\n", url);
	}

	return text;
}

/**
 * The one row, whose text says what pressing it does now.
 */
static const char *menutextCommunityAction(struct menuitem *item)
{
	const u32 size = communityGetSize();
	u32 done;
	u32 total;

	if (!communityIsAvailable()) {
		return "Not available in this build\n";
	}

	switch (communityGetState()) {
	case COMMUNITY_ASKING:
		return "Cancel\n";
	case COMMUNITY_DOWNLOAD:
		communityGetProgress(&done, &total);

		// Megabytes rather than a percentage, for the reason the update page
		// gives: what a player wants when it is slow is how much is left, and
		// the total is worth seeing before deciding to wait for it.
		if (total > 0) {
			snprintf(g_CommunityText, sizeof(g_CommunityText), "Cancel (%u.%u of %u.%u MB)\n",
					done / 1048576, (done % 1048576) * 10 / 1048576,
					total / 1048576, (total % 1048576) * 10 / 1048576);

			return g_CommunityText;
		}

		return "Cancel\n";
	case COMMUNITY_UNPACKING:
		return "Unpacking...\n";
	case COMMUNITY_FOUND:
		snprintf(g_CommunityText, sizeof(g_CommunityText), "Download and Install (%u MB)\n",
				(size + 524288) / 1048576);

		return g_CommunityText;
	case COMMUNITY_DONE:
		return "Installed\n";
	default:
		break;
	}

	return communityIsInstalled(communityGetSelected())
		? "Check for a Newer Version\n"
		: "Find Latest Release\n";
}

static MenuItemHandlerResult menuhandlerCommunityAction(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const s32 state = communityGetState();

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		// Unpacking is the one thing here that cannot be stopped part way -
		// archiveExtract() writes a few thousand files and has nowhere to be
		// asked to give up - and an install that is done is not a button.
		return !communityIsAvailable() || state == COMMUNITY_UNPACKING || state == COMMUNITY_DONE;
	case MENUOP_SET:
		switch (state) {
		case COMMUNITY_ASKING:
		case COMMUNITY_DOWNLOAD:
			communityCancel();
			break;
		case COMMUNITY_FOUND:
			communityInstall();
			break;
		default:
			communityCheck();
			break;
		}
		break;
	}

	return 0;
}

/**
 * What is happening, or what happened. The pack folder is named while nothing
 * is: "where did it go" is the next question after an install and the answer
 * is not obvious from a menu that has already moved on.
 */
static const char *menutextCommunityStatus(struct menuitem *item)
{
	static char text[224];
	const char *status = communityGetStatus();

	if (status[0]) {
		snprintf(text, sizeof(text), "%s\n", status);
	} else if (communityIsInstalled(communityGetSelected())) {
		snprintf(text, sizeof(text), "Installed. Look for a newer one, or use it from the\npage behind this one.\n");
	} else {
		snprintf(text, sizeof(text), "Packs install into texture-packs and are switched on\nfor you.\n");
	}

	return text;
}

/**
 * The cover art, drawn into the blank rows this item reserves.
 *
 * The rectangle is the item's own y and the dialog's own middle: a label is
 * given the width of the column rather than of the window, so centring on
 * renderdata->width would put the picture off to one side of the page.
 */
static MenuItemHandlerResult menuhandlerCommunityPoster(s32 operation, struct menuitem *item, union handlerdata *data)
{
	struct menudialog *dialog = g_Menus[g_MpPlayerNum].curdialog;
	struct menuitemrenderdata *renderdata;
	Gfx *gdl;
	s32 textheight;
	s32 textwidth;
	s32 width;
	s32 height;
	s32 x1;
	s32 y1;

	if (operation != MENUOP_RENDER) {
		return 0;
	}

	gdl = data->type19.gdl;
	renderdata = data->type19.renderdata2;

	if (dialog == NULL) {
		return (intptr_t)gdl;
	}

	// The row's own height, from the row's own text. Guessed at first, and the
	// guess was two lines out - the picture drew over the button underneath
	// it, which is a layout that looks like a rendering fault.
	textMeasure(&textheight, &textwidth, (char *)item->param2,
			g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);

	height = textheight - 2;
	width = COMMUNITY_POSTERW(height);

	x1 = dialog->x + (dialog->width - width) / 2;
	y1 = renderdata->y + 1;

	// A frame a pixel wider than the picture on every side, in the menu's own
	// colours, so that a dark cover does not float in the window with no edge.
	gdl = menugfxDrawFilledRect(gdl, x1 - 1, y1 - 1, x1 + width + 1, y1 + height + 1,
			renderdata->colour, renderdata->colour);

	gdl = menuImageDraw(gdl, communityGetThumb(communityGetSelected()),
			x1, y1, x1 + width, y1 + height, 255);

	return (intptr_t)gdl;
}

/**
 * Which pack. Hidden while there is only one of them, because a dropdown with
 * one option in it reads as a control that is broken.
 */
static MenuItemHandlerResult menuhandlerCommunityPick(s32 operation, struct menuitem *item, union handlerdata *data)
{
	switch (operation) {
	case MENUOP_CHECKHIDDEN:
		return communityGetNumPacks() < 2;
	case MENUOP_GETOPTIONCOUNT:
		data->dropdown.value = communityGetNumPacks();
		break;
	case MENUOP_GETOPTIONTEXT:
		return (intptr_t)communityGetName(data->dropdown.value);
	case MENUOP_SET:
		communitySetSelected((s32)data->dropdown.value);
		break;
	case MENUOP_GETSELECTEDINDEX:
		data->dropdown.value = communityGetSelected();
	}

	return 0;
}

/**
 * Ask on the way in, once. A page that opens saying nothing and waits to be
 * told to ask is a page with an extra press in front of everything it does,
 * and the request is one small reply.
 */
static MenuDialogHandlerResult menudialogCommunity(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_OPEN && communityIsAvailable()) {
		// Every opening, not only the first: the answer is a release that
		// somebody else moves, and an install that has finished would
		// otherwise leave the page saying "Installed" with no way to ask
		// whether there is a newer one. A check while one is running is
		// refused by community.c rather than guarded here.
		communityCheck();
	}

	return 0;
}

/**
 * The download bar, in the blank row at the bottom of the page.
 *
 * Drawn after the dialogs from menuRenderDialogs(), for the reason the update
 * page's bar is: a menu item cannot draw and this is not one. See
 * updatemenuRenderProgress(), which this is the twin of.
 */
#define COMMUNITY_BARINSET  8
#define COMMUNITY_BARHEIGHT 5

Gfx *communitymenuRenderProgress(Gfx *gdl)
{
	const struct menucolourpalette *colours = &g_MenuColours[MENUDIALOGTYPE_DEFAULT];
	struct menudialog *dialog = g_Menus[g_MpPlayerNum].curdialog;
	s32 viewleft = viGetViewLeft() / g_ScaleX;
	s32 viewtop = viGetViewTop();
	u32 done;
	u32 total;
	s32 x1;
	s32 x2;
	s32 y1;
	s32 y2;
	s32 fill;

	if (dialog == NULL || dialog->definition != &g_CommunityMenuDialog) {
		return gdl;
	}

	if (communityGetState() != COMMUNITY_DOWNLOAD) {
		return gdl;
	}

	communityGetProgress(&done, &total);

	if (total == 0) {
		return gdl;
	}

	if (done > total) {
		done = total;
	}

	x1 = dialog->x + COMMUNITY_BARINSET;
	x2 = dialog->x + dialog->width - COMMUNITY_BARINSET;
	y2 = dialog->y + dialog->height - 4;
	y1 = y2 - COMMUNITY_BARHEIGHT;

	// In the wide type before it is cut down: two hundred million times a
	// width overflows a u32 at about four thousand.
	fill = (s32)((u64)(x2 - x1) * done / total);

	g_MenuScissorX1 = viewleft;
	g_MenuScissorY1 = viewtop;
	g_MenuScissorX2 = (viGetViewLeft() + viGetViewWidth()) / g_ScaleX;
	g_MenuScissorY2 = viewtop + viGetViewHeight();
	gdl = menuApplyScissor(gdl);

	gdl = menugfxDrawFilledRect(gdl, x1, y1, x2, y2,
			(colours->item_unfocused & 0xffffff00) | 0x40,
			(colours->item_unfocused & 0xffffff00) | 0x40);

	if (fill > 0) {
		gdl = menugfxDrawFilledRect(gdl, x1, y1, x1 + fill, y2,
				colours->item_focused_inner, colours->item_focused_outer);
	}

	return gdl;
}

struct menuitem g_CommunityMenuItems[] = {
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextCommunityPack,
		0,
		NULL,
	},
	{
		// The poster. Its text is the space it takes and nothing else - see
		// the note over menuhandlerCommunityPoster().
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LIST_CUSTOMRENDER,
		(uintptr_t)COMMUNITY_POSTERTEXT,
		0,
		menuhandlerCommunityPoster,
	},
	{
		MENUITEMTYPE_DROPDOWN,
		0,
		MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)"Pack",
		0,
		menuhandlerCommunityPick,
	},
	{
		MENUITEMTYPE_SELECTABLE,
		0,
		0,
		(uintptr_t)&menutextCommunityAction,
		0,
		menuhandlerCommunityAction,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextCommunityStatus,
		0,
		NULL,
	},
	{
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT,
		(uintptr_t)&menutextCommunitySource,
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
	{
		// The row the progress bar is drawn in, and an empty line the rest of
		// the time - the only way to reserve space for something that is not
		// a menu item. See the same row on the update page.
		MENUITEMTYPE_LABEL,
		0,
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT | MENUITEMFLAG_LITERAL_TEXT,
		(uintptr_t)" \n",
		0,
		NULL,
	},
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_CommunityMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Community Packs",
	g_CommunityMenuItems,
	menudialogCommunity,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};
