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
 * A page per pack, swiped left and right the way Dab's Mod Options is: each is
 * a sibling dialog on the same layer (menudialogdef.nextsibling), so a Left or
 * Right that no row takes turns the page and the chevrons beside the dialog
 * name the packs either side. Each page is what the pack is, who made it, what
 * it looks like, and one row that says what pressing it does now - ask,
 * download, or nothing because it is already installed. The work is all in
 * community.c and all on a worker thread; this polls, the way the update page
 * polls updateGetState().
 *
 * Every page is drawn while a swipe is under way, so nothing here may read
 * "the pack being looked at": an item carries its pack in its `param`, and the
 * pages are otherwise the same list of rows.
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
// spare without scrolling, and only while the rest of the page is as short as
// it can be - see menutextCommunityPoster().
#define COMMUNITY_POSTERTEXT "\n\n\n\n\n\n\n\n\n\n"
#define COMMUNITY_POSTERLINES 10

// The small-font lines the rest of the page was measured with at ten: the
// pack's name, author and three lines of blurb, one line of status and the
// source over two.
#define COMMUNITY_POSTERBUDGET "\n\n\n\n\n\n\n\n"

// Two thirds, which is the shape of the art a pack is announced with.
#define COMMUNITY_POSTERW(h) ((h) * 2 / 3)

// One page per pack in community.c's catalogue, in its order.
#define COMMUNITY_NUMPAGES 3

#define communitymenuPack(item) ((s32)(item)->param)

static const char *menutextCommunityPack(struct menuitem *item)
{
	static char text[256];
	const s32 index = communitymenuPack(item);

	snprintf(text, sizeof(text), "%s\nby %s\n%s",
			communityGetName(index), communityGetAuthor(index), communityGetBlurb(index));

	return text;
}

/**
 * Where the pack came from, so that anybody who would rather do this by hand -
 * or wants to read what they are about to download - has the address.
 */
static const char *menutextCommunitySource(struct menuitem *item)
{
	static char text[192];

	const char *url = communityGetSource(communitymenuPack(item));
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
	static char text[64];
	const s32 index = communitymenuPack(item);
	u32 done;
	u32 total;
	u32 size;

	if (!communityIsAvailable()) {
		return "Not available in this build\n";
	}

	switch (communityGetState(index)) {
	case COMMUNITY_ASKING:
		return "Cancel\n";
	case COMMUNITY_DOWNLOAD:
		communityGetProgress(&done, &total);

		// Megabytes rather than a percentage, for the reason the update page
		// gives: what a player wants when it is slow is how much is left, and
		// the total is worth seeing before deciding to wait for it.
		if (total > 0) {
			snprintf(text, sizeof(text), "Cancel (%u.%u of %u.%u MB)\n",
					done / 1048576, (done % 1048576) * 10 / 1048576,
					total / 1048576, (total % 1048576) * 10 / 1048576);

			return text;
		}

		return "Cancel\n";
	case COMMUNITY_UNPACKING:
		return "Unpacking...\n";
	case COMMUNITY_FOUND:
		size = communityGetSize(index);
		snprintf(text, sizeof(text), "Download and Install (%u MB)\n", (size + 524288) / 1048576);

		return text;
	case COMMUNITY_DONE:
		return "Installed\n";
	case COMMUNITY_ELSEWHERE:
		return "Another Pack Is Installing\n";
	default:
		break;
	}

	return communityIsInstalled(index)
		? "Check for a Newer Version\n"
		: "Find Latest Release\n";
}

static MenuItemHandlerResult menuhandlerCommunityAction(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const s32 index = communitymenuPack(item);
	const s32 state = communityGetState(index);

	switch (operation) {
	case MENUOP_CHECKDISABLED:
		// Unpacking is the one thing here that cannot be stopped part way -
		// archiveExtract() writes a few thousand files and has nowhere to be
		// asked to give up - and an install that is done is not a button. Nor
		// is another pack's download, which is stopped from its own page.
		return !communityIsAvailable() || state == COMMUNITY_UNPACKING || state == COMMUNITY_DONE
			|| state == COMMUNITY_ELSEWHERE;
	case MENUOP_SET:
		switch (state) {
		case COMMUNITY_ASKING:
		case COMMUNITY_DOWNLOAD:
			communityCancel();
			break;
		case COMMUNITY_FOUND:
			communityInstall(index);
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
	const s32 index = communitymenuPack(item);
	const char *status = communityGetStatus(index);

	if (status[0]) {
		snprintf(text, sizeof(text), "%s\n", status);
	} else if (communityIsInstalled(index)) {
		snprintf(text, sizeof(text), "Installed. Look for a newer one, or use it from the\npage behind this one.\n");
	} else {
		snprintf(text, sizeof(text), "Packs install into texture-packs and are switched on\nfor you.\n");
	}

	return text;
}

static s32 communitymenuMeasureSmall(const char *text)
{
	s32 textheight = 0;
	s32 textwidth = 0;

	textMeasure(&textheight, &textwidth, (char *)text, g_CharsHandelGothicXs, g_FontHandelGothicXs, 0);

	return textheight;
}

/**
 * The poster's blank lines: ten, less whatever the rest of the page has grown.
 *
 * The page is exactly as tall as a dialog may be with ten, so one more line
 * anywhere - the two line status an installed pack shows - makes it scroll to
 * keep the button in view, and the scroll takes the top of the first line off
 * under the title bar. The ROM's glyphs hide two units of that in their empty
 * top rows; the release's font fills its band to the top and shows it. So the
 * picture gives the room back: a line of its own for every part of a line the
 * other rows took.
 */
static const char *menutextCommunityPoster(struct menuitem *item)
{
	s32 lineheight = 0;
	s32 textwidth = 0;
	s32 extra;
	s32 lines = COMMUNITY_POSTERLINES;

	textMeasure(&lineheight, &textwidth, "\n", g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);

	extra = communitymenuMeasureSmall(menutextCommunityPack(item))
		+ communitymenuMeasureSmall(menutextCommunityStatus(item))
		+ communitymenuMeasureSmall(menutextCommunitySource(item))
		- communitymenuMeasureSmall(COMMUNITY_POSTERBUDGET);

	if (extra > 0 && lineheight > 0) {
		lines -= (extra + lineheight - 1) / lineheight;
	}

	if (lines < 1) {
		lines = 1;
	}

	return COMMUNITY_POSTERTEXT + COMMUNITY_POSTERLINES - lines;
}

extern struct menudialogdef *g_CommunityPageDialogs[COMMUNITY_NUMPAGES];

/**
 * The cover art, drawn into the blank rows this item reserves.
 *
 * The rectangle is the item's own y and its own dialog's middle: a label is
 * given the width of the column rather than of the window, so centring on
 * renderdata->width would put the picture off to one side of the page. And
 * the dialog is this item's page and not the current one, because the page
 * being swiped away is drawn too.
 */
static MenuItemHandlerResult menuhandlerCommunityPoster(s32 operation, struct menuitem *item, union handlerdata *data)
{
	const s32 index = communitymenuPack(item);
	struct menudialog *dialog;
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
	dialog = index < COMMUNITY_NUMPAGES ? menuIsDialogOpen(g_CommunityPageDialogs[index]) : NULL;

	if (dialog == NULL) {
		return (intptr_t)gdl;
	}

	// The row's own height, from the row's own text. Guessed at first, and the
	// guess was two lines out - the picture drew over the button underneath
	// it, which is a layout that looks like a rendering fault.
	textMeasure(&textheight, &textwidth, menuResolveParam2Text(item),
			g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);

	height = textheight - 2;
	width = COMMUNITY_POSTERW(height);

	x1 = dialog->x + (dialog->width - width) / 2;
	y1 = renderdata->y + 1;

	// A frame a pixel wider than the picture on every side, in the menu's own
	// colours, so that a dark cover does not float in the window with no edge.
	gdl = menugfxDrawFilledRect(gdl, x1 - 1, y1 - 1, x1 + width + 1, y1 + height + 1,
			renderdata->colour, renderdata->colour);

	gdl = menuImageDraw(gdl, communityGetThumb(index), x1, y1, x1 + width, y1 + height, 255);

	return (intptr_t)gdl;
}

/**
 * Ask on the way in, once. A page that opens saying nothing and waits to be
 * told to ask is a page with an extra press in front of everything it does,
 * and the request is one small reply.
 */
static MenuDialogHandlerResult menudialogCommunity(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (operation == MENUOP_OPEN && communityIsAvailable()) {
		// Every page is opened together and each is told so; the first asks,
		// for all of them, and community.c refuses the others while it does.
		communityCheck();
	}

	return 0;
}

/**
 * The download bar, in the blank row at the bottom of the page.
 *
 * Drawn after the dialogs from menuRenderDialogs(), for the reason the update
 * page's bar is: a menu item cannot draw and this is not one. See
 * updatemenuRenderProgress(), which this is the twin of. Only on the page of
 * the pack being downloaded.
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
	s32 active;

	if (dialog == NULL) {
		return gdl;
	}

	active = communityGetActivePack();

	if (active < 0 || active >= COMMUNITY_NUMPAGES || dialog->definition != g_CommunityPageDialogs[active]) {
		return gdl;
	}

	if (communityGetState(active) != COMMUNITY_DOWNLOAD) {
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

/**
 * A page's rows, the same on every page but for the pack each one is about.
 */
#define COMMUNITY_PAGEITEMS(pack) { \
	{ \
		MENUITEMTYPE_LABEL, \
		pack, \
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, \
		(uintptr_t)&menutextCommunityPack, \
		0, \
		NULL, \
	}, \
	{ \
		/* The poster. Its text is the space it takes and nothing else - */ \
		/* see the note over menuhandlerCommunityPoster(). */ \
		MENUITEMTYPE_LABEL, \
		pack, \
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_LIST_CUSTOMRENDER, \
		(uintptr_t)&menutextCommunityPoster, \
		0, \
		menuhandlerCommunityPoster, \
	}, \
	{ \
		MENUITEMTYPE_SELECTABLE, \
		pack, \
		0, \
		(uintptr_t)&menutextCommunityAction, \
		0, \
		menuhandlerCommunityAction, \
	}, \
	{ \
		MENUITEMTYPE_LABEL, \
		pack, \
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, \
		(uintptr_t)&menutextCommunityStatus, \
		0, \
		NULL, \
	}, \
	{ \
		MENUITEMTYPE_LABEL, \
		pack, \
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, \
		(uintptr_t)&menutextCommunitySource, \
		0, \
		NULL, \
	}, \
	{ \
		MENUITEMTYPE_SEPARATOR, \
		0, \
		0, \
		0, \
		0, \
		NULL, \
	}, \
	{ \
		MENUITEMTYPE_SELECTABLE, \
		0, \
		MENUITEMFLAG_SELECTABLE_CLOSESDIALOG, \
		L_OPTIONS_213, /* "Back" */ \
		0, \
		NULL, \
	}, \
	{ \
		/* The row the progress bar is drawn in, and an empty line the */ \
		/* rest of the time - the only way to reserve space for something */ \
		/* that is not a menu item. See the same row on the update page. */ \
		MENUITEMTYPE_LABEL, \
		0, \
		MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT | MENUITEMFLAG_LITERAL_TEXT, \
		(uintptr_t)" \n", \
		0, \
		NULL, \
	}, \
	{ MENUITEMTYPE_END }, \
}

static struct menuitem g_CommunityUltimateMenuItems[] = COMMUNITY_PAGEITEMS(0);
static struct menuitem g_CommunityXblaMenuItems[] = COMMUNITY_PAGEITEMS(1);
static struct menuitem g_CommunityForeverMenuItems[] = COMMUNITY_PAGEITEMS(2);

// The chain is declared last-to-first so each page can name the next.
static struct menudialogdef g_CommunityForeverMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Community: Forever Plus",
	g_CommunityForeverMenuItems,
	menudialogCommunity,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};

static struct menudialogdef g_CommunityXblaMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Community: XBLA Plus",
	g_CommunityXblaMenuItems,
	menudialogCommunity,
	MENUDIALOGFLAG_LITERAL_TEXT,
	&g_CommunityForeverMenuDialog,
};

// The head of the chain, and the one Texture Packs opens: the recommended pack.
struct menudialogdef g_CommunityMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Community: Ultimate Plus",
	g_CommunityUltimateMenuItems,
	menudialogCommunity,
	MENUDIALOGFLAG_LITERAL_TEXT,
	&g_CommunityXblaMenuDialog,
};

struct menudialogdef *g_CommunityPageDialogs[COMMUNITY_NUMPAGES] = {
	&g_CommunityMenuDialog,
	&g_CommunityXblaMenuDialog,
	&g_CommunityForeverMenuDialog,
};
