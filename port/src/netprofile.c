#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "platform.h"
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "system.h"
#include "game/menu.h"
#include "game/mplayer/mplayer.h"
#include "config.h"
#include "net/netlobby.h"
#include "netprofile.h"

/*
 * The online player profile (the owner, F3 20261012-115801: "option to copy
 * offline player settings to online, and saves online settings so dont need
 * to change every session"; with 20261012-121828/-122036: online, Player
 * Setup's Name row and Team Control said "Player 1" for a player signed in
 * as "Dabba").
 *
 * What a seat plays online by is player 1's config (netWriteSlotCfg sends
 * its character, Control Style and option bits; the rest of the seat's own
 * settings are pd.ini's [Game.Player1] and NETKEY_PLAYER keys, which persist
 * already). Player 1's config is the Combat Simulator's, which nothing keeps
 * between sessions unless a player file is loaded, so online it was the
 * defaults every time unless set again.
 *
 * While the online pages are up - Name Entry opens them (the Perfect Menu's
 * Online Game, a mode's Combat Simulator, the way back after a match) and its
 * Back closes them - player 1 is the online profile: these fields from
 * pd.ini's [Net], everything else the offline player's, its name the lobby
 * account's, and no player file behind it (an online match's end never
 * saves into the offline player's file). The offline player comes back
 * whole as Name Entry closes. The first time, the online profile is the
 * offline player's as it stands. A match in between keeps it: menuStop()
 * closes no dialog, and the net rules put back what they took over.
 */

static s32 s_Saved = 0;       // Net.PlayerProfile: 0 none kept yet
static s32 s_Body = 0;        // Net.PlayerBody: mpbodynum
static s32 s_Head = 0;        // Net.PlayerHead: mpheadnum
static s32 s_ControlMode = 0; // Net.PlayerControlMode: Control Style
static s32 s_Options = 0;     // Net.PlayerOptions: OPTION_* (Control and Player Options)

static s32 s_Active = 0;
static struct mpplayerconfig s_Offline; // player 1 as it was offline, while the online profile plays
static u64 s_CopiedAt = 0;              // Copy From Offline Player's word on its row

PD_CONSTRUCTOR static void netProfileConfigInit(void)
{
	configRegisterInt("Net.PlayerProfile", &s_Saved, 0, 1);
	configRegisterInt("Net.PlayerBody", &s_Body, 0, 255);
	configRegisterInt("Net.PlayerHead", &s_Head, 0, 255);
	configRegisterInt("Net.PlayerControlMode", &s_ControlMode, 0, 255);
	configRegisterInt("Net.PlayerOptions", &s_Options, 0, 0xffff);
}

static void netProfileFromPlayer(const struct mpplayerconfig *p)
{
	s_Body = p->base.mpbodynum;
	s_Head = p->base.mpheadnum;
	s_ControlMode = p->controlmode;
	s_Options = p->options;
	s_Saved = 1;
}

// A character this game has no longer (its conversion or mod gone) stays
// the offline player's
static void netProfileToPlayer(struct mpplayerconfig *p)
{
	if (s_Body < (s32)mpGetNumBodies()) {
		p->base.mpbodynum = s_Body;
	}

	if (mpHeadNumSafe(s_Head) == s_Head) {
		p->base.mpheadnum = s_Head;
	}

	p->controlmode = s_ControlMode;
	p->options = s_Options;
}

static s32 netProfileDiffers(const struct mpplayerconfig *p)
{
	return p->base.mpbodynum != s_Body || p->base.mpheadnum != s_Head
		|| p->controlmode != s_ControlMode || p->options != s_Options;
}

s32 netProfileActive(void)
{
	return s_Active;
}

void netProfileEnter(void)
{
	struct mpplayerconfig *p = &g_PlayerConfigsArray[0];

	if (s_Active) {
		return;
	}

	s_Offline = *p;
	s_Active = 1;

	if (!s_Saved) {
		netProfileFromPlayer(p);
	} else {
		netProfileToPlayer(p);
	}

	memset(&p->fileguid, 0, sizeof(p->fileguid));
	netProfileTick();

	sysLogPrintf(LOG_NOTE, "netprofile: player 1 plays the online profile: body %d, head %d, control style %d, options 0x%04x",
			p->base.mpbodynum, p->base.mpheadnum, p->controlmode, p->options);
}

void netProfileLeave(void)
{
	struct mpplayerconfig *p = &g_PlayerConfigsArray[0];

	if (!s_Active) {
		return;
	}

	netProfileFromPlayer(p);
	*p = s_Offline;
	s_Active = 0;
	configSave(CONFIG_PATH);

	sysLogPrintf(LOG_NOTE, "netprofile: the online profile kept (body %d, head %d, control style %d, options 0x%04x); player 1 is the offline player again",
			s_Body, s_Head, s_ControlMode, s_Options);
}

/**
 * Online, player 1's name is the account's (as a lobby host's own match
 * already had it, netsession.c): Player Setup's Name row, Team Control and
 * every other list of the setup's players say it
 */
void netProfileTick(void)
{
	const char *account = netLobbyAccount();
	char *dst = g_PlayerConfigsArray[0].base.name;
	char name[sizeof(g_PlayerConfigsArray[0].base.name)];
	s32 n = 0;

	if (!s_Active || !account || !account[0]) {
		return;
	}

	while (account[n] && n < (s32)sizeof(name) - 2) {
		name[n] = account[n] >= 0x20 && account[n] < 0x7f ? account[n] : '?';
		n++;
	}

	name[n++] = '\n';
	name[n] = '\0';

	if (strcmp(dst, name) != 0) {
		memcpy(dst, name, n + 1);
	}
}

/*
 * The online pages' Player Setup (Name Entry's and the Game Lobby's Player
 * Setup... rows): the Combat Simulator's pages for the character and the
 * controls, the name the account's and not edited here, and the offline
 * player's settings to start from
 */

extern struct menudialogdef g_MpCharacterMenuDialog;

static char *netProfileTextName(struct menuitem *item)
{
	static char name[sizeof(g_PlayerConfigsArray[0].base.name) + 1];

	if (s_Active) {
		return g_PlayerConfigsArray[0].base.name;
	}

	snprintf(name, sizeof(name), "%s\n", netLobbyAccount());

	return name;
}

static MenuItemHandlerResult handlerProfileName(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return true;
	}

	return 0;
}

static char *netProfileTextCopy(struct menuitem *item)
{
	if (s_CopiedAt && sysGetMicroseconds() - s_CopiedAt < 2000000) {
		return "Copied From Offline Player\n";
	}

	return "Copy From Offline Player\n";
}

static MenuItemHandlerResult handlerProfileCopy(s32 operation, struct menuitem *item, union handlerdata *data)
{
	if (operation == MENUOP_CHECKDISABLED) {
		return !s_Active;
	}

	if (operation == MENUOP_SET && s_Active) {
		struct mpplayerconfig *p = &g_PlayerConfigsArray[0];

		p->base.mpbodynum = s_Offline.base.mpbodynum;
		p->base.mpheadnum = s_Offline.base.mpheadnum;
		p->controlmode = s_Offline.controlmode;
		p->options = s_Offline.options;
		netProfileFromPlayer(p);
		configSave(CONFIG_PATH);
		s_CopiedAt = sysGetMicroseconds();

		sysLogPrintf(LOG_NOTE, "netprofile: copied from the offline player: body %d, head %d, control style %d, options 0x%04x",
				s_Body, s_Head, s_ControlMode, s_Options);
	}

	return 0;
}

static MenuDialogHandlerResult dialogProfile(s32 operation, struct menudialogdef *dialogdef, union handlerdata *data)
{
	if (!s_Active) {
		return 0;
	}

	// what the Character, Control and Player Options pages changed: kept
	// for the next session (and pd.ini's write at the game's exit)
	if (operation == MENUOP_TICK && netProfileDiffers(&g_PlayerConfigsArray[0])) {
		netProfileFromPlayer(&g_PlayerConfigsArray[0]);
	}

	if (operation == MENUOP_CLOSE) {
		netProfileFromPlayer(&g_PlayerConfigsArray[0]);
		configSave(CONFIG_PATH);
	}

	return 0;
}

static struct menuitem s_ProfileItems[] = {
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_LITERAL_TEXT, (uintptr_t)"Name\n", (uintptr_t)&netProfileTextName, handlerProfileName },
	{ MENUITEMTYPE_LABEL, 0, MENUITEMFLAG_LITERAL_TEXT | MENUITEMFLAG_LESSLEFTPADDING | MENUITEMFLAG_SMALLFONT, (uintptr_t)"Online you play as your account,\nset on Name Entry.\n", 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG, L_MPMENU_031, 0, (void *)&g_MpCharacterMenuDialog }, // "Character"
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG, L_MPMENU_033, 0, (void *)&g_MpControlMenuDialog }, // "Control"
	{ MENUITEMTYPE_SELECTABLE, 0, MENUITEMFLAG_SELECTABLE_OPENSDIALOG, L_MPMENU_034, 0, (void *)&g_MpPlayerOptionsMenuDialog }, // "Player Options"
	{ MENUITEMTYPE_SEPARATOR, 0, 0, 0, 0, NULL },
	{ MENUITEMTYPE_SELECTABLE, 0, 0, (uintptr_t)&netProfileTextCopy, 0, handlerProfileCopy },
	{ MENUITEMTYPE_END },
};

struct menudialogdef g_NetPlayerSetupMenuDialog = {
	MENUDIALOGTYPE_DEFAULT,
	(uintptr_t)"Player Setup",
	s_ProfileItems,
	dialogProfile,
	MENUDIALOGFLAG_LITERAL_TEXT,
	NULL,
};
