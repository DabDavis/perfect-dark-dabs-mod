#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "platform.h"
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "config.h"
#include "system.h"
#include "input.h"
#include "lib/vi.h"
#include "game/game_1531a0.h"
#include "game/mplayer/setup.h"
#include "game/menu.h"
#include "game/chraction.h"
#include "game/modspectate.h"
#include "game/playermgr.h"
#include "game/player.h"
#include "game/prop.h"
#include "game/propobj.h"
#include "game/radar.h"
#include "net/net.h"
#include "gecinema.h"
#include "netint.h"

/**
 * The online HUD (protocol 18; netproto.h CHAT and PLAYERS).
 *
 * Three things over the local player's view in a net match, and one in a
 * lobby room's Game Lobby:
 *
 *   - the feed, at the top left: the host's notices (a player joined the
 *     match in progress, left, lost the connection, came back to the seat
 *     kept for it, started watching; with the seats played out of the
 *     match's, "3/8") and everyone's chat. A line stays ten seconds; with
 *     the chat line open the last ten stay, however old.
 *   - the chat line: the chat key (Net.ChatKey, T) opens it, and so does
 *     Space when none of player 1's binds uses it (Net.ChatSpace). The
 *     keyboard types into it at once, with no keyboard to pick on screen:
 *     Enter says it, Escape drops it. While it is open the keyboard's keys
 *     are letters (input.c drops the keyboard's binds while text input is
 *     on, as it does for a menu's keyboard), and Escape is the line's, not
 *     the pause menu's: netHudFrame runs right after inputUpdate, before
 *     anything else asks for ESC's press, and takes it.
 *   - the players panel, while the players key (Net.PlayersKey, P) is held
 *     (not while the line is open, where P is a letter): every seat of the
 *     match played or kept,
 *     each one's score and deaths in a Combat Simulator match, its ping
 *     (the host's own measure, sent in PLAYERS once a second), the open
 *     seats and the spectators, and the count.
 *   - a player's name over its head while the crosshair is on it
 *     (Net.PlayerNames), in a Combat Simulator match and on a co-op mission
 *     alike: who plays each of the other characters. The crosshair's own
 *     query (propFindAimingAt, as the stock HUD asks it with one player), so
 *     a wall or a cloak hides the name as it would the shot.
 *   - the Game Lobby (netlobbymenu.c): Space or the chat key there opens the
 *     same line, for the room's chat on pdlobbyd; its status row shows it.
 *
 * Pad players get the panel and their chat from the pause menu's Players
 * page (netlobbymenu.c), which this file's netHudSay sends for.
 *
 * Nothing here runs offline: netHudFrame reads no key unless a net match's
 * stage or the Game Lobby is up, and only through
 * inputKeyPressedThisFrame(), which spends nothing another reader of the
 * same key would have had.
 */

#define FEED_MAX        32
#define FEED_SHOW       6     // lines shown while the line is shut ...
#define FEED_SHOW_OPEN  10    // ... and open
#define FEED_LIFE_MS    10000 // a line's time on screen
#define FEED_FADE_MS    1000  // the last of it, fading

#define HUDKIND_HINT    3     // a line of this machine's own (the keys)

#define CHAT_SHUT       0
#define CHAT_MATCH      1     // the match's chat (netSessionChatSend)
#define CHAT_LOBBY      2     // the lobby room's (netLobbyChat)

struct feedline {
	u64 at;
	s32 kind;   // NETCHAT_*, HUDKIND_HINT
	s32 from;   // the sender's seat or spectator view, NETCHAT_FROM_NONE
	char name[NET_MAXNAME + 1];
	char text[NET_MAXCHAT + 1];
	// the text wrapped for the width it was last drawn at
	s32 wrapw;
	char wrapped[NET_MAXCHAT * 2 + 8];
	s32 width;
	s32 height;
};

static struct feedline s_Feed[FEED_MAX];
static s32 s_FeedHead = 0; // the oldest
static s32 s_FeedLen = 0;

static s32 s_ChatOpen = CHAT_SHUT;
static char s_ChatText[NET_MAXCHAT + 1];
static s32 s_ChatLen = 0;

static s32 s_PanelHeld = 0;
static u32 s_EscFrame = 0; // the frame whose ESC press the line took
static s32 s_LiveWas = 0;
static u32 s_Frame = 1;
static u32 s_LobbyFrame = 0;

static char s_ChatKeyName[32] = "T";
static char s_PlayersKeyName[32] = "P";
static s32 s_ChatSpace = 1;
static s32 s_PlayerNames = 1;
static s32 s_ChatVk = -1;
static s32 s_PlayersVk = -1;
static char s_ChatKeyShown[48];

// harness: --net-test-chat "TICK:TEXT|TICK:TEXT", --net-test-chat-type
// "TICK:TEXT" (the line opened with TEXT in it), --net-test-players TICK
// (the panel from then on), --net-test-lobby-chat-type TEXT (the Game
// Lobby's line, once it is up)
#define TESTCHAT_MAX 8
static struct {
	u32 tick;
	char text[NET_MAXCHAT + 1];
	s32 done;
} s_TestChat[TESTCHAT_MAX];
static s32 s_NumTestChat = 0;
static u32 s_TestTypeTick = 0;
static char s_TestTypeText[NET_MAXCHAT + 1];
static u32 s_TestPanelTick = 0;
static u32 s_TestPageTick = 0;   // --net-test-players-page TICK: the pause's Control page, then Players
static char s_TestLobbyType[NET_MAXCHAT + 1];
static u32 s_TestLobbySendAt = 0; // ... and said half a second later
// --net-test-chat-echo: a headless joiner answers every person's line (not
// another bot's: "netbot" names), so a player has someone to talk to
static s32 s_TestEcho = 0;
static char s_EchoPending[NET_MAXCHAT + 1];

// the room's last chat line as last logged (the gates read "net: lobby chat:")
static char s_LobbyLast[NETLOBBY_MAXUSER + NETLOBBY_MAXCHATTEXT + 8];

PD_CONSTRUCTOR static void netHudConfigInit(void)
{
	configRegisterString("Net.ChatKey", s_ChatKeyName, sizeof(s_ChatKeyName));
	configRegisterString("Net.PlayersKey", s_PlayersKeyName, sizeof(s_PlayersKeyName));
	configRegisterInt("Net.ChatSpace", &s_ChatSpace, 0, 1);
	configRegisterInt("Net.PlayerNames", &s_PlayerNames, 0, 1);
}

static s32 netHudKey(const char *name, s32 *vk)
{
	if (*vk < 0) {
		*vk = !name[0] || strcmp(name, "NONE") == 0 ? 0 : inputGetKeyByName(name);

		if (*vk < 0) {
			*vk = 0;
		}
	}

	return *vk;
}

static u64 netHudNowMs(void)
{
	return sysGetMicroseconds() / 1000;
}

static s32 netHudParseTick(const char *arg, u32 *tick, char *text, s32 size)
{
	const char *colon = arg ? strchr(arg, ':') : NULL;

	if (!colon) {
		return 0;
	}

	*tick = (u32)strtoul(arg, NULL, 10);
	snprintf(text, size, "%s", colon + 1);

	return 1;
}

void netHudArgs(void)
{
	const char *chat = sysArgGetString("--net-test-chat");
	const char *type = sysArgGetString("--net-test-chat-type");
	const char *lobby = sysArgGetString("--net-test-lobby-chat-type");

	if (chat) {
		char all[TESTCHAT_MAX * (NET_MAXCHAT + 16)];
		char *part = all;

		snprintf(all, sizeof(all), "%s", chat);

		while (part && s_NumTestChat < TESTCHAT_MAX) {
			char *bar = strchr(part, '|');

			if (bar) {
				*bar = '\0';
			}

			if (netHudParseTick(part, &s_TestChat[s_NumTestChat].tick, s_TestChat[s_NumTestChat].text, sizeof(s_TestChat[0].text))) {
				s_NumTestChat++;
			}

			part = bar ? bar + 1 : NULL;
		}
	}

	if (type && !netHudParseTick(type, &s_TestTypeTick, s_TestTypeText, sizeof(s_TestTypeText))) {
		s_TestTypeTick = 0;
	}

	s_TestPanelTick = (u32)sysArgGetInt("--net-test-players", 0);
	s_TestPageTick = (u32)sysArgGetInt("--net-test-players-page", 0);
	s_TestEcho = sysArgCheck("--net-test-chat-echo");

	if (lobby) {
		snprintf(s_TestLobbyType, sizeof(s_TestLobbyType), "%s", lobby);
	}
}

/**
 * A line as the wire may carry it and the font can draw it: printable ASCII,
 * the rest dropped, no space at either end. Returns its length.
 */
s32 netChatClean(char *text)
{
	s32 len = 0;
	s32 i;

	for (i = 0; text[i]; i++) {
		const u8 c = (u8)text[i];

		if (c >= 0x20 && c < 0x7f && (len > 0 || c != ' ')) {
			text[len++] = (char)c;
		}
	}

	while (len > 0 && text[len - 1] == ' ') {
		len--;
	}

	text[len] = '\0';

	return len;
}

static struct feedline *netHudFeedAt(s32 i)
{
	return &s_Feed[(s_FeedHead + i) % FEED_MAX];
}

static void netHudPush(s32 kind, s32 from, const char *name, const char *text)
{
	struct feedline *line;

	if (s_FeedLen == FEED_MAX) {
		s_FeedHead = (s_FeedHead + 1) % FEED_MAX;
		s_FeedLen--;
	}

	line = netHudFeedAt(s_FeedLen++);
	memset(line, 0, sizeof(*line));
	line->at = netHudNowMs();
	line->kind = kind;
	line->from = from;
	snprintf(line->name, sizeof(line->name), "%s", name ? name : "");
	snprintf(line->text, sizeof(line->text), "%s", text);
}

/**
 * A line for the feed: a player's (NETCHAT_PLAYER, from its seat or
 * spectator view), the host's notice, or the host's word to this machine
 * alone. Each goes in the log as well, the way the gates read them.
 */
void netHudFeed(s32 kind, s32 from, const char *name, const char *text)
{
	if (kind == NETCHAT_PLAYER) {
		sysLogPrintf(LOG_NOTE, "net: chat: %s: %s", name, text);
	} else {
		sysLogPrintf(LOG_NOTE, "net: notice: %s", text);
	}

	netHudPush(kind, from, name, text);

	if (s_TestEcho && kind == NETCHAT_PLAYER && from != g_NetLocalSlot && strncmp(name, "netbot", 6) != 0) {
		snprintf(s_EchoPending, sizeof(s_EchoPending), "%s said: %s", name, text);
	}
}

// Whether Space is free for the chat in a match: no bind of player 1's uses it
static s32 netHudSpaceFree(void)
{
	s32 ck;
	s32 i;

	if (!s_ChatSpace) {
		return 0;
	}

	for (ck = 0; ck < CK_TOTAL_COUNT; ck++) {
		const u32 *binds = inputKeyGetBinds(0, ck);

		for (i = 0; binds && i < INPUT_MAX_BINDS; i++) {
			if (binds[i] == VK_SPACE) {
				return 0;
			}
		}
	}

	return 1;
}

const char *netHudChatKeyName(void)
{
	const s32 vk = netHudKey(s_ChatKeyName, &s_ChatVk);

	if (vk > 0 && netHudSpaceFree() && vk != VK_SPACE) {
		snprintf(s_ChatKeyShown, sizeof(s_ChatKeyShown), "%s or SPACE", inputGetKeyName(vk));
	} else if (vk > 0) {
		snprintf(s_ChatKeyShown, sizeof(s_ChatKeyShown), "%s", inputGetKeyName(vk));
	} else {
		snprintf(s_ChatKeyShown, sizeof(s_ChatKeyShown), "%s", netHudSpaceFree() ? "SPACE" : "");
	}

	return s_ChatKeyShown;
}

static void netHudChatStart(s32 mode, const char *text)
{
	s_ChatOpen = mode;
	snprintf(s_ChatText, sizeof(s_ChatText), "%s", text ? text : "");
	s_ChatLen = strlen(s_ChatText);
	inputStartTextInput();
}

static void netHudChatStop(s32 say)
{
	const s32 mode = s_ChatOpen;

	s_ChatOpen = CHAT_SHUT;

	// the keys held now (Enter) stay up until let go: Enter is not the
	// menu's OK, nor START in the N64 binds
	if (inputIsTextInputActive()) {
		inputStopTextInput();
	}

	if (!say || netChatClean(s_ChatText) == 0) {
		return;
	}

	if (mode == CHAT_LOBBY) {
		netLobbyChat(s_ChatText);
	} else if (netSessionChatSend(s_ChatText) != 0) {
		netHudPush(NETCHAT_PRIVATE, NETCHAT_FROM_NONE, "", "Not sent: the host is not there.");
	}

	s_ChatText[0] = '\0';
	s_ChatLen = 0;
}

/**
 * The open line's keys: the characters typed since the last frame,
 * Backspace (Ctrl takes a word), Ctrl+V, Enter and Escape
 */
static void netHudChatType(void)
{
	const s32 ctrl = (inputGetKeyModState() & KM_CTRL) != 0;
	s32 key;
	char ch;

	// a screen that stopped the typing (a dialog's own keyboard) shut it
	if (!inputIsTextInputActive()) {
		s_ChatOpen = CHAT_SHUT;
		return;
	}

	// ESC is the line's: whatever asks for its press after this frame's
	// inputUpdate (the pause, a spectator's leave, a menu's back) is told no;
	// what reads the frame's press instead asks netHudAteEscape()
	inputKeyJustPressed(VK_ESCAPE);

	if (inputKeyPressedThisFrame(VK_ESCAPE)) {
		s_EscFrame = s_Frame;
	}

	while (!ctrl && (ch = inputGetLastTextChar()) != 0) {
		inputClearLastTextChar();

		if ((u8)ch >= 0x20 && (u8)ch < 0x7f && s_ChatLen < NET_MAXCHAT) {
			s_ChatText[s_ChatLen++] = ch;
			s_ChatText[s_ChatLen] = '\0';
		}
	}

	key = inputGetLastKey();
	inputClearLastKey();

	if (ctrl && key == VK_A + ('v' - 'a')) {
		const char *clip = inputGetClipboard();

		if (clip) {
			s32 i;

			for (i = 0; clip[i] && s_ChatLen < NET_MAXCHAT; i++) {
				if ((u8)clip[i] >= 0x20 && (u8)clip[i] < 0x7f) {
					s_ChatText[s_ChatLen++] = clip[i];
				}
			}

			s_ChatText[s_ChatLen] = '\0';
			inputClearClipboard();
		}
	} else if (key == VK_BACKSPACE && s_ChatLen > 0) {
		if (ctrl) {
			while (s_ChatLen > 0 && s_ChatText[s_ChatLen - 1] == ' ') {
				s_ChatLen--;
			}

			while (s_ChatLen > 0 && s_ChatText[s_ChatLen - 1] != ' ') {
				s_ChatLen--;
			}
		} else {
			s_ChatLen--;
		}

		s_ChatText[s_ChatLen] = '\0';
	} else if (key == VK_RETURN || key == VK_KEYBOARD_BEGIN + SDL_SCANCODE_KP_ENTER) {
		netHudChatStop(1);
	} else if (key == VK_ESCAPE) {
		netHudChatStop(0);
	}
}

// The harness's lines and its open line, on this machine's ticks
static void netHudTestTick(s32 live)
{
	s32 i;

	if (!live) {
		return;
	}

	for (i = 0; i < s_NumTestChat; i++) {
		if (!s_TestChat[i].done && g_NetTick >= s_TestChat[i].tick) {
			s_TestChat[i].done = 1;
			sysLogPrintf(LOG_NOTE, "net: --net-test-chat: saying \"%s\" at tick %u", s_TestChat[i].text, g_NetTick);
			netSessionChatSend(s_TestChat[i].text);
		}
	}

	if (s_TestTypeTick && g_NetTick >= s_TestTypeTick && s_ChatOpen == CHAT_SHUT) {
		s_TestTypeTick = 0;
		netHudChatStart(CHAT_MATCH, s_TestTypeText);
	}
}

extern struct menudialogdef g_MpPauseControlMenuDialog;
extern struct menudialogdef g_NetPlayersMenuDialog;

/**
 * At the end of a tick (net.c): --net-test-players-page opens this machine's
 * pause menu on its Control page (a Combat Simulator match), with its
 * Players row, and three seconds on the Players page over it, the way a pad
 * gets there
 */
void netHudTick(void)
{
	const s32 pn = g_NetLocalSlot;
	s32 prev;

	if (!s_TestPageTick || !netSessionHudLive() || pn < 0 || pn >= PLAYERCOUNT() || !g_Vars.normmplayerisrunning) {
		return;
	}

	if (g_NetTick != s_TestPageTick && g_NetTick != s_TestPageTick + 180) {
		return;
	}

	prev = g_MpPlayerNum;
	g_MpPlayerNum = g_Vars.playerstats[pn].mpindex;

	if (g_NetTick == s_TestPageTick && g_Menus[g_MpPlayerNum].curdialog == NULL) {
		g_Menus[g_MpPlayerNum].playernum = pn;
		menuPushRootDialog(&g_MpPauseControlMenuDialog, MENUROOT_MPPAUSE);
		sysLogPrintf(LOG_NOTE, "net: --net-test-players-page: the Control page at tick %u", g_NetTick);
	} else if (g_NetTick == s_TestPageTick + 180 && g_Menus[g_MpPlayerNum].curdialog) {
		menuPushDialog(&g_NetPlayersMenuDialog);
		sysLogPrintf(LOG_NOTE, "net: --net-test-players-page: the Players page at tick %u", g_NetTick);
	}

	g_MpPlayerNum = prev;
}

/**
 * Once a frame, right after inputUpdate (pdsched.c): the line's keys while
 * it is open, else the keys that open it; the players key; the hint at the
 * start of a match. Offline it reads nothing.
 */
void netHudFrame(void)
{
	const s32 live = g_NetMode != NETMODE_NONE && netSessionHudLive();
	const s32 lobby = !live && s_LobbyFrame && s_Frame - s_LobbyFrame <= 2 && netLobbyInRoom();

	s_Frame++;

	// offline: nothing to read, and the last session's lines went with it
	if (g_NetMode == NETMODE_NONE && !g_NetLobbyRoom) {
		s_FeedLen = 0;
		s_LiveWas = 0;
		s_PanelHeld = 0;

		if (s_ChatOpen != CHAT_SHUT) {
			netHudChatStop(0);
		}

		return;
	}

	if (live && !s_LiveWas) {
		const char *chatkey = netHudChatKeyName();
		const s32 playersvk = netHudKey(s_PlayersKeyName, &s_PlayersVk);
		char hint[NET_MAXCHAT + 1];

		if (chatkey[0] && playersvk > 0) {
			snprintf(hint, sizeof(hint), "%s: chat    hold %s: players", chatkey, inputGetKeyName(playersvk));
		} else if (chatkey[0]) {
			snprintf(hint, sizeof(hint), "%s: chat", chatkey);
		} else {
			hint[0] = '\0';
		}

		if (hint[0]) {
			netHudPush(HUDKIND_HINT, NETCHAT_FROM_NONE, "", hint);
		}
	}

	s_LiveWas = live;

	if (s_ChatOpen != CHAT_SHUT && !(s_ChatOpen == CHAT_MATCH ? live : lobby)) {
		// the match or the Game Lobby went from under the line
		netHudChatStop(0);
	}

	if (s_ChatOpen != CHAT_SHUT) {
		netHudChatType();
	} else if ((live || lobby) && !inputIsTextInputActive() && g_MenuKeyboardPlayer < 0) {
		const s32 vk = netHudKey(s_ChatKeyName, &s_ChatVk);
		const s32 space = inputKeyPressedThisFrame(VK_SPACE) && (lobby || netHudSpaceFree());

		if ((vk > 0 && inputKeyPressedThisFrame(vk)) || space) {
			netHudChatStart(live ? CHAT_MATCH : CHAT_LOBBY, NULL);
		} else if (lobby && s_TestLobbyType[0]) {
			netHudChatStart(CHAT_LOBBY, s_TestLobbyType);
			s_TestLobbyType[0] = '\0';
			s_TestLobbySendAt = s_Frame + 30;
			sysLogPrintf(LOG_NOTE, "net: --net-test-lobby-chat-type: the Game Lobby's line open");
		}
	}

	if (s_TestLobbySendAt && s_Frame >= s_TestLobbySendAt && s_ChatOpen == CHAT_LOBBY) {
		s_TestLobbySendAt = 0;
		netHudChatStop(1);
	}

	// the room's chat as it comes, for the log
	if (g_NetLobbyRoom && netLobbyInRoom()) {
		const struct netlobbyroom *room = netLobbyGetRoom();

		if (room->valid && room->nchat > 0) {
			char last[sizeof(s_LobbyLast)];

			snprintf(last, sizeof(last), "%s: %s", room->chat[room->nchat - 1].user, room->chat[room->nchat - 1].text);

			if (strcmp(last, s_LobbyLast) != 0) {
				snprintf(s_LobbyLast, sizeof(s_LobbyLast), "%s", last);
				sysLogPrintf(LOG_NOTE, "net: lobby chat: %s", last);
			}
		}
	}

	{
		const s32 vk = netHudKey(s_PlayersKeyName, &s_PlayersVk);

		// held: not while typing, where P is a letter
		s_PanelHeld = live && s_ChatOpen == CHAT_SHUT && vk > 0 && inputKeyPressed(vk);
	}

	netHudTestTick(live);

	if (s_EchoPending[0] && live) {
		netSessionChatSend(s_EchoPending);
		s_EchoPending[0] = '\0';
	}
}

s32 netHudChatOpen(void)
{
	return s_ChatOpen != CHAT_SHUT;
}

// This frame's ESC press went to the chat line (GoldenEye's watch and cinema
// read the frame's press, not inputKeyJustPressed's)
s32 netHudAteEscape(void)
{
	return s_ChatOpen != CHAT_SHUT || s_EscFrame == s_Frame;
}

void netHudLobbyFrame(void)
{
	s_LobbyFrame = s_Frame;
}

/**
 * The open line for a row of the Game Lobby: "Say: " and as much of its tail
 * as `maxchars` lets in, with a cursor. 0 while it is shut.
 */
s32 netHudChatLine(char *out, s32 size, s32 maxchars)
{
	const char *tail = s_ChatText;
	const s32 cursor = (netHudNowMs() / 500) & 1;

	if (s_ChatOpen != CHAT_LOBBY) {
		return 0;
	}

	if (s_ChatLen > maxchars) {
		tail = s_ChatText + s_ChatLen - maxchars;
	}

	snprintf(out, size, "Say: %s%s%s\n", s_ChatLen > maxchars ? "..." : "", tail, cursor ? "_" : " ");

	return 1;
}

// A line said from a menu (the pause menu's Players page): the match's
// chat in a match, else the lobby room's
void netHudSay(const char *text)
{
	char line[NET_MAXCHAT + 1];

	snprintf(line, sizeof(line), "%s", text);

	if (netChatClean(line) == 0) {
		return;
	}

	if (g_NetMode != NETMODE_NONE && netSessionHudLive()) {
		if (netSessionChatSend(line) != 0) {
			netHudPush(NETCHAT_PRIVATE, NETCHAT_FROM_NONE, "", "Not sent: the host is not there.");
		}
	} else if (netLobbyInRoom()) {
		netLobbyChat(line);
	}
}

void netHudCounts(s32 *in, s32 *of, s32 *specs)
{
	char name[NET_MAXNAME + 1];
	s32 ping;

	netSessionSeatCounts(in, of);

	for (*specs = 0; *specs < NET_MAXSPECS && netSessionSpecInfo(*specs, name, sizeof(name), &ping); *specs += 1) {
	}
}

s32 netHudPlayerRow(s32 k, char *name, s32 namesize, char *value, s32 valuesize)
{
	struct netseatinfo info;
	s32 ping;
	s32 i;

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (!netSessionSeatInfo(i, &info) || info.state == NETSEATINFO_OPEN || k-- > 0) {
			continue;
		}

		snprintf(name, namesize, "%s%s\n", info.name, info.local ? " (you)" : info.state == NETSEATINFO_HELD ? " (away)" : "");

		if (info.state == NETSEATINFO_HOST) {
			snprintf(value, valuesize, "%s", "host\n");
		} else if (info.state == NETSEATINFO_HELD) {
			snprintf(value, valuesize, "%s", "away\n");
		} else if (info.ping >= 0) {
			snprintf(value, valuesize, "%d ms\n", info.ping);
		} else {
			snprintf(value, valuesize, "%s", "...\n");
		}

		return 1;
	}

	for (i = 0; i < NET_MAXSPECS; i++) {
		char spec[NET_MAXNAME + 1];

		if (!netSessionSpecInfo(i, spec, sizeof(spec), &ping)) {
			break;
		}

		if (k-- > 0) {
			continue;
		}

		snprintf(name, namesize, "%s (watching)\n", spec);

		if (ping >= 0) {
			snprintf(value, valuesize, "%d ms\n", ping);
		} else {
			snprintf(value, valuesize, "%s", "...\n");
		}

		return 1;
	}

	return 0;
}

/*
 * The name over a player under the crosshair
 */

#define TAG_HOLD_MS 250 // the name stays this long after the crosshair leaves its player ...
#define TAG_FADE_MS 200 // ... then fades over this

static s32 s_TagPlayer = -1; // the player the crosshair was last on
static u64 s_TagSeenAt = 0;  // when
static u32 s_TagFrame = 0;   // the frame s_TagX/Y were worked out for (s_Frame)
static f32 s_TagX = 0;       // the top middle of its box on screen
static f32 s_TagY = 0;

// The player whose chr `prop` is, when this machine's player may be shown
// its name: another player, living, drawn (no cloak but to the IR scanner,
// not spectating); -1 otherwise
static s32 netHudTagPlayerOf(struct prop *prop)
{
	struct chrdata *chr;
	s32 pn;

	if (!prop || prop->type != PROPTYPE_PLAYER || !prop->chr) {
		return -1;
	}

	chr = prop->chr;
	pn = playermgrGetPlayerNumByProp(prop);

	if (pn < 0 || pn >= MAX_PLAYERS || pn == g_Vars.currentplayernum || !g_Vars.players[pn]
			|| g_Vars.players[pn]->isdead || chrIsDead(chr) || !modSpectatePropNoticeable(prop)) {
		return -1;
	}

	if ((chr->hidden & CHRHFLAG_CLOAKED) && !USINGDEVICE(DEVICE_IRSCANNER)) {
		return -1;
	}

	// a body this camera stands in is not drawn (playerGetNetBodyAlphaFrac:
	// a co-op mission's players start on one spot), nor named
	if (playerGetNetBodyAlphaFrac(prop) <= 0) {
		return -1;
	}

	return pn;
}

/**
 * lvRender, in this machine's player's pass once the props are posed for its
 * camera (after the stock lookingatprop): which player the crosshair is on,
 * and where that player's box is on screen. The stock HUD asks only with one
 * player (or co-op), so the same query is made here for every net match.
 */
void netHudAimFrame(void)
{
	struct player *player = g_Vars.currentplayer;
	struct prop *aimed;
	s32 hadinfo[2];
	struct coord dotpos[2];
	struct coord dotrot[2];
	u64 now;
	s32 pn;
	s32 i;

	if (!netIsLocalSlot(g_Vars.currentplayernum)) {
		return; // a remote player's pass on the host
	}

	if (!s_PlayerNames || !netSessionHudLive() || netSessionSpectating()
			|| netCoopCinemaKind() != GECINEMA_NET_NONE
			|| player->isdead || g_Vars.tickmode == TICKMODE_CUTSCENE
			|| player->cameramode == CAMERAMODE_EYESPY
			|| modSpectateIsOnForPlayer(g_Vars.currentplayernum)) {
		s_TagPlayer = -1;
		return;
	}

	// The query sets the gun's dot (the laser sight's, a thrown gun's aim)
	// where it lands: put back as bgunAimThrowAtCrosshair does, so the sim
	// is the same as without it
	for (i = 0; i < 2; i++) {
		hadinfo[i] = player->hands[i].hasdotinfo;
		dotpos[i] = player->hands[i].dotpos;
		dotrot[i] = player->hands[i].dotrot;
	}

	aimed = propFindAimingAt(HAND_RIGHT, false, FINDPROPCONTEXT_QUERY);

	for (i = 0; i < 2; i++) {
		player->hands[i].hasdotinfo = hadinfo[i];
		player->hands[i].dotpos = dotpos[i];
		player->hands[i].dotrot = dotrot[i];
	}

	now = netHudNowMs();
	pn = netHudTagPlayerOf(aimed);

	if (pn >= 0) {
		s_TagPlayer = pn;
		s_TagSeenAt = now;
	} else if (s_TagPlayer >= 0 && now - s_TagSeenAt >= TAG_HOLD_MS + TAG_FADE_MS) {
		s_TagPlayer = -1;
	}

	if (s_TagPlayer >= 0) {
		struct prop *prop = g_Vars.players[s_TagPlayer] ? g_Vars.players[s_TagPlayer]->prop : NULL;
		f32 x1;
		f32 x2;
		f32 y1;
		f32 y2;

		if (netHudTagPlayerOf(prop) != s_TagPlayer) {
			s_TagPlayer = -1; // died, cloaked or left while the name stayed
			return;
		}

		// the box the stock target box is drawn from (lvUpdateTrackedProp)
		if ((prop->flags & PROPFLAG_ONTHISSCREENTHISTICK) && prop->chr->model
				&& modelGetScreenCoords(prop->chr->model, &x2, &x1, &y2, &y1)) {
			s_TagX = (x1 + x2) * 0.5f;
			s_TagY = y1;
			s_TagFrame = s_Frame;
		}
	}
}

/*
 * Drawing
 */

#define COL_NAME     0x8fd8ffff
#define COL_OWNNAME  0x9cff9cff
#define COL_SPECNAME 0xc0c0c0ff
#define COL_TEXT     0xffffffff
#define COL_NOTICE   0xffd25aff
#define COL_PRIVATE  0xff8a70ff
#define COL_HINT     0xb0b0b0ff
#define COL_BACK     0x00000080
#define COL_PANEL    0x000000b0
#define COL_DIM      0x909090ff

static u32 netHudFade(u32 colour, s32 alpha)
{
	return (colour & 0xffffff00) | (((colour & 0xff) * alpha / 255) & 0xff);
}

static void netHudMeasure(const char *text, s32 *w, s32 *h)
{
	textMeasure(h, w, (char *)text, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);
}

/**
 * `in` folded to `width` into `out`, each line ending in "\n": broken at
 * the last space that fits, or inside a word longer than the line
 */
static void netHudWrap(const char *in, char *out, s32 size, s32 width)
{
	char line[NET_MAXCHAT + 2];
	const char *p = in;
	s32 o = 0;

	out[0] = '\0';

	while (*p && o < size - 2) {
		s32 lastspace = -1;
		s32 n = 0;
		s32 fit;

		while (p[n] && n < NET_MAXCHAT) {
			s32 w;
			s32 h;

			memcpy(line, p, n + 1);
			line[n + 1] = '\0';
			netHudMeasure(line, &w, &h);

			if (w > width && n > 0) {
				break;
			}

			if (p[n] == ' ') {
				lastspace = n;
			}

			n++;
		}

		fit = p[n] && lastspace > 0 ? lastspace : n;

		if (fit <= 0) {
			fit = 1;
		}

		if (o + fit + 2 > size) {
			fit = size - o - 2;
		}

		memcpy(out + o, p, fit);
		o += fit;
		out[o++] = '\n';
		out[o] = '\0';
		p += fit;

		while (*p == ' ') {
			p++;
		}
	}
}

static Gfx *netHudText(Gfx *gdl, s32 x, s32 y, const char *text, u32 colour, s32 alpha)
{
	char buf[NET_MAXCHAT * 2 + 64];

	snprintf(buf, sizeof(buf), "%s", text);

	return textRender(gdl, &x, &y, buf, g_CharsHandelGothicSm, g_FontHandelGothicSm,
			netHudFade(colour, alpha), netHudFade(0x000000ff, alpha), viGetWidth(), viGetHeight(), 0, 0);
}

static u32 netHudLineColour(const struct feedline *line)
{
	switch (line->kind) {
	case NETCHAT_NOTICE:  return COL_NOTICE;
	case NETCHAT_PRIVATE: return COL_PRIVATE;
	case HUDKIND_HINT:    return COL_HINT;
	default:              return COL_TEXT;
	}
}

static u32 netHudNameColour(const struct feedline *line)
{
	if (line->from >= MAX_PLAYERS && line->from < NET_MAXVIEWS) {
		return COL_SPECNAME;
	}

	if (line->from == g_NetLocalSlot && !netSessionSpectating()) {
		return COL_OWNNAME;
	}

	return COL_NAME;
}

/**
 * The feed at the view's top left, oldest line first, and under its newest
 * the open line; from `top` (the panel's foot while it is up) down to no
 * lower than 60% of the view: the oldest of the lines shown go first. The
 * stock HUD's own messages (pickups, kills) sit at the bottom left.
 */
static Gfx *netHudRenderFeed(Gfx *gdl, s32 top)
{
	const u64 now = netHudNowMs();
	const s32 open = s_ChatOpen == CHAT_MATCH;
	const s32 left = viGetViewLeft() + 10;
	const s32 maxw = viGetViewWidth() * 60 / 100;
	const s32 bottom = viGetViewTop() + viGetViewHeight() * 60 / 100;
	s32 lineh;
	s32 dummy;
	s32 room;
	s32 shown[FEED_SHOW_OPEN];
	s32 nshown = 0;
	s32 total = 0;
	s32 y;
	s32 i;

	netHudMeasure("Ay\n", &dummy, &lineh);

	// the open line goes under the feed: its room is kept
	room = bottom - top - (open ? lineh + 6 : 0);

	for (i = s_FeedLen - 1; i >= 0 && nshown < (open ? FEED_SHOW_OPEN : FEED_SHOW); i--) {
		struct feedline *line = netHudFeedAt(i);
		char prefix[NET_MAXNAME + 4];
		s32 prefixw = 0;
		s32 h;

		if (!open && now - line->at >= FEED_LIFE_MS) {
			break;
		}

		if (line->kind == NETCHAT_PLAYER) {
			snprintf(prefix, sizeof(prefix), "%s: ", line->name);
			netHudMeasure(prefix, &prefixw, &h);
		}

		if (line->wrapw != maxw) {
			line->wrapw = maxw;
			netHudWrap(line->text, line->wrapped, sizeof(line->wrapped), maxw - prefixw);
			netHudMeasure(line->wrapped, &line->width, &line->height);
			line->width += prefixw;
		}

		if (total + line->height + 2 > room) {
			break;
		}

		shown[nshown++] = i;
		total += line->height + 2;
	}

	y = top;

	for (i = nshown - 1; i >= 0; i--) {
		struct feedline *line = netHudFeedAt(shown[i]);
		const u64 age = now - line->at;
		s32 alpha = 255;
		s32 x = left;

		if (!open && age > FEED_LIFE_MS - FEED_FADE_MS) {
			alpha = (s32)((FEED_LIFE_MS - age) * 255 / FEED_FADE_MS);
			alpha = alpha < 0 ? 0 : alpha;
		}

		gdl = text0f153a34(gdl, left - 3, y - 1, left + line->width + 3, y + line->height + 1, netHudFade(COL_BACK, alpha));

		if (line->kind == NETCHAT_PLAYER) {
			char prefix[NET_MAXNAME + 4];
			s32 w;
			s32 h;

			snprintf(prefix, sizeof(prefix), "%s: ", line->name);
			netHudMeasure(prefix, &w, &h);
			gdl = netHudText(gdl, x, y, prefix, netHudNameColour(line), alpha);
			x += w;
		}

		gdl = netHudText(gdl, x, y, line->wrapped, netHudLineColour(line), alpha);
		y += line->height + 2;
	}

	if (open) {
		// the open line: its tail, as much as fits, and a cursor (drawn:
		// the font's '_' sits at the top of the cell)
		char say[NET_MAXCHAT + 48];
		const char *tail = s_ChatText;
		const s32 cursor = ((now / 500) & 1) == 0;
		s32 boxw;
		s32 w;
		s32 h;

		for (;;) {
			snprintf(say, sizeof(say), "Say: %s%s\n", tail != s_ChatText ? "..." : "", tail);
			netHudMeasure(say, &w, &h);

			if (w <= maxw - 6 || !*tail) {
				break;
			}

			tail++;
		}

		boxw = w + 4;

		if (s_ChatLen == 0) {
			s32 hintw;

			netHudMeasure("Say:    Enter: send   Esc: cancel\n", &hintw, &h);
			boxw = hintw;
		}

		y += 4;
		gdl = text0f153a34(gdl, left - 3, y - 1, left + boxw + 3, y + lineh + 1, COL_PANEL);
		gdl = netHudText(gdl, left, y, say, COL_TEXT, 255);

		if (s_ChatLen == 0) {
			s32 sayw;

			netHudMeasure("Say:    \n", &sayw, &h);
			gdl = netHudText(gdl, left + sayw, y, "Enter: send   Esc: cancel\n", COL_HINT, 255);
		}

		if (cursor) {
			gdl = text0f153a34(gdl, left + w + 1, y + 1, left + w + 3, y + lineh - 2, COL_TEXT);
		}
	}

	return gdl;
}

static Gfx *netHudRightText(Gfx *gdl, s32 right, s32 y, const char *text, u32 colour)
{
	s32 w;
	s32 h;

	netHudMeasure(text, &w, &h);

	return netHudText(gdl, right - w, y, text, colour, 255);
}

static s32 netHudTextWidth(const char *text)
{
	s32 w;
	s32 h;

	netHudMeasure(text, &w, &h);

	return w;
}

static u32 netHudPingColour(s32 ping)
{
	return ping < 80 ? 0x9cff9cff : ping < 150 ? 0xffe070ff : 0xff7060ff;
}

/**
 * The players panel, at the top of the view: a row per seat played or
 * kept, then the open seats and the spectators on a line each. Returns the
 * panel's foot.
 */
static Gfx *netHudRenderPanel(Gfx *gdl, s32 *foot)
{
	const s32 mp = g_Vars.normmplayerisrunning;
	struct netseatinfo seats[MAX_PLAYERS];
	s32 rows = 0;
	s32 open = 0;
	s32 in = 0;
	s32 of = 0;
	s32 nspecs = 0;
	char specline[160];
	char text[96];
	s32 namew = netHudTextWidth("MMMMMMMMMMMM\n");
	s32 pingw = netHudTextWidth("9999 ms\n");
	s32 deathsw = netHudTextWidth("DEATHS\n");
	s32 scorew = netHudTextWidth("SCORE\n");
	s32 width;
	s32 x0;
	s32 x1;
	s32 colping;
	s32 coldeaths;
	s32 colscore;
	s32 lineh;
	s32 dummy;
	s32 y;
	s32 i;

	netHudMeasure("Ay\n", &dummy, &lineh);

	for (i = 0; i < MAX_PLAYERS; i++) {
		if (!netSessionSeatInfo(i, &seats[i])) {
			seats[i].state = 0;
			continue;
		}

		of++;

		if (seats[i].state == NETSEATINFO_OPEN) {
			open++;
		} else {
			s32 w = netHudTextWidth(seats[i].name) + netHudTextWidth(" (away)");

			rows++;
			in += seats[i].state != NETSEATINFO_HELD;
			namew = w > namew ? w : namew;
		}
	}

	specline[0] = '\0';

	for (i = 0; i < NET_MAXSPECS; i++) {
		char name[NET_MAXNAME + 1];
		s32 ping;
		s32 len = strlen(specline);

		if (!netSessionSpecInfo(i, name, sizeof(name), &ping)) {
			break;
		}

		if (ping >= 0) {
			snprintf(specline + len, sizeof(specline) - len, "%s%s (%d ms)", nspecs ? ", " : "Watching: ", name, ping);
		} else {
			snprintf(specline + len, sizeof(specline) - len, "%s%s", nspecs ? ", " : "Watching: ", name);
		}

		nspecs++;
	}

	// as wide as its columns, at most the view less a margin
	width = 12 + namew + 14 + pingw + (mp ? scorew + deathsw + 28 : 0);
	width = width > viGetViewWidth() - 16 ? viGetViewWidth() - 16 : width;
	x0 = viGetViewLeft() + (viGetViewWidth() - width) / 2;
	x1 = x0 + width;
	colping = x1 - 6;
	coldeaths = colping - pingw - 14;
	colscore = coldeaths - deathsw - 14;

	y = viGetViewTop() + 24;
	*foot = y + lineh * (rows + 1 + (open > 0) + (nspecs > 0)) + 10;
	gdl = text0f153a34(gdl, x0, y - 4, x1, *foot - 2, COL_PANEL);
	gdl = text0f153a34(gdl, x0, y - 4, x1, y - 3, COL_NOTICE);

	snprintf(text, sizeof(text), "PLAYERS %d/%d\n", in, of);
	gdl = netHudText(gdl, x0 + 6, y, text, COL_NOTICE, 255);

	if (mp) {
		gdl = netHudRightText(gdl, colscore, y, "SCORE\n", COL_NOTICE);
		gdl = netHudRightText(gdl, coldeaths, y, "DEATHS\n", COL_NOTICE);
	}

	gdl = netHudRightText(gdl, colping, y, "PING\n", COL_NOTICE);
	y += lineh + 2;

	for (i = 0; i < MAX_PLAYERS; i++) {
		const struct netseatinfo *s = &seats[i];
		const s32 held = s->state == NETSEATINFO_HELD;
		char name[NET_MAXNAME + 16];
		u32 colour = s->local ? COL_OWNNAME : COL_TEXT;

		if (s->state == 0 || s->state == NETSEATINFO_OPEN) {
			continue;
		}

		snprintf(name, sizeof(name), "%s%s\n", s->name, held ? " (away)" : "");
		gdl = netHudText(gdl, x0 + 6, y, name, held ? COL_DIM : colour, 255);

		if (mp) {
			s32 score = 0;
			s32 deaths = 0;

			scenarioCalculatePlayerScore(MPCHR(i), i, &score, &deaths);
			snprintf(text, sizeof(text), "%d\n", score);
			gdl = netHudRightText(gdl, colscore, y, text, held ? COL_DIM : COL_TEXT);
			snprintf(text, sizeof(text), "%d\n", deaths);
			gdl = netHudRightText(gdl, coldeaths, y, text, held ? COL_DIM : COL_TEXT);
		}

		if (s->state == NETSEATINFO_HOST) {
			gdl = netHudRightText(gdl, colping, y, "host\n", COL_NOTICE);
		} else if (held) {
			gdl = netHudRightText(gdl, colping, y, "away\n", COL_DIM);
		} else if (s->ping >= 0) {
			snprintf(text, sizeof(text), "%d ms\n", s->ping);
			gdl = netHudRightText(gdl, colping, y, text, netHudPingColour(s->ping));
		} else {
			gdl = netHudRightText(gdl, colping, y, "...\n", COL_DIM);
		}

		y += lineh;
	}

	if (open > 0) {
		snprintf(text, sizeof(text), open == 1 ? "1 open seat\n" : "%d open seats\n", open);
		gdl = netHudText(gdl, x0 + 6, y, text, COL_DIM, 255);
		y += lineh;
	}

	if (nspecs > 0) {
		char line[176];

		snprintf(line, sizeof(line), "%s\n", specline);
		gdl = netHudText(gdl, x0 + 6, y, line, COL_SPECNAME, 255);
	}

	return gdl;
}

// How much of the name under the crosshair shows this frame (0 none)
static s32 netHudTagAlpha(void)
{
	u64 age;

	if (s_TagPlayer < 0 || s_TagFrame != s_Frame) {
		return 0;
	}

	age = netHudNowMs() - s_TagSeenAt;

	if (age <= TAG_HOLD_MS) {
		return 255;
	}

	if (age >= TAG_HOLD_MS + TAG_FADE_MS) {
		return 0;
	}

	return (s32)(255 * (TAG_HOLD_MS + TAG_FADE_MS - age) / TAG_FADE_MS);
}

/**
 * The name of the player under the crosshair, centred over the top of its
 * box: the seat's name, as the panel and the chat show it; its team's
 * colour (lightened, to read over the level) in a team match
 */
static Gfx *netHudRenderTag(Gfx *gdl, s32 alpha)
{
	const s32 left = viGetViewLeft();
	const s32 top = viGetViewTop();
	const s32 right = left + viGetViewWidth();
	const s32 bottom = top + viGetViewHeight();
	struct netseatinfo info;
	char text[NET_MAXNAME + 2];
	u32 colour = COL_NAME;
	s32 x;
	s32 y;
	s32 w;
	s32 h;

	if (!netSessionSeatInfo(s_TagPlayer, &info) || !info.name[0]) {
		return gdl;
	}

	if (g_Vars.normmplayerisrunning && (g_MpSetup.options & MPOPTION_TEAMSENABLED)) {
		const u32 team = g_TeamColours[g_PlayerConfigsArray[g_Vars.playerstats[s_TagPlayer].mpindex].base.team & 7];
		u32 r = team >> 24 & 0xff;
		u32 g = team >> 16 & 0xff;
		u32 b = team >> 8 & 0xff;

		r = r + (255 - r) * 2 / 5;
		g = g + (255 - g) * 2 / 5;
		b = b + (255 - b) * 2 / 5;
		colour = r << 24 | g << 16 | b << 8 | 0xff;
	}

	snprintf(text, sizeof(text), "%s\n", info.name);
	netHudMeasure(text, &w, &h);

	x = (s32)s_TagX - w / 2;
	y = (s32)s_TagY - h - 4;

	if (x > right - w - 2) {
		x = right - w - 2;
	}

	if (x < left + 2) {
		x = left + 2;
	}

	if (y > bottom - h - 2) {
		y = bottom - h - 2;
	}

	if (y < top + 2) {
		y = top + 2;
	}

	return netHudText(gdl, x, y, text, colour, alpha);
}

/**
 * lvRender, over the local player's view after its HUD, menus and modal
 * text: the name under the crosshair, the panel and the feed. Nothing on a view this machine does not
 * show (a remote player's pass on the host), nor before GO.
 */
/**
 * Before GO: what this machine waits on (netSessionWaitLine: a load, a
 * download of the stage's folder), centred a third of the way down, over the
 * stage's first frame - or the menus a download runs under. Once a frame, on
 * the local player's view.
 */
static u32 s_WaitFrame = 0;

static Gfx *netHudRenderWait(Gfx *gdl)
{
	char line[NET_MAXTEXT + 128];
	s32 w;
	s32 h;
	s32 x;
	s32 y;

	if (s_WaitFrame == s_Frame || !netSessionWaitLine(line, sizeof(line))) {
		return gdl;
	}

	// a match's stage builds every player's view: this machine's own
	if (netSessionMatchActive() && !netIsLocalSlot(g_Vars.currentplayernum)) {
		return gdl;
	}

	s_WaitFrame = s_Frame;
	netHudMeasure(line, &w, &h);
	x = viGetViewLeft() + (viGetViewWidth() - w) / 2;
	y = viGetViewTop() + viGetViewHeight() / 3;

	gdl = text0f153628(gdl);
	gdl = netHudText(gdl, x, y, line, COL_NOTICE, 0xff);
	gdl = text0f153780(gdl);

	return gdl;
}

void *netHudRender(void *gdlp)
{
	Gfx *gdl = gdlp;
	const s32 panel = s_PanelHeld || (s_TestPanelTick && g_NetTick >= s_TestPanelTick);
	s32 top = viGetViewTop() + 12;
	s32 tag;

	if (!netSessionHudLive() || !netIsLocalSlot(g_Vars.currentplayernum)) {
		return netHudRenderWait(gdl);
	}

	tag = netHudTagAlpha();

	if (!panel && s_FeedLen == 0 && !tag) {
		return gdl;
	}

	gdl = text0f153628(gdl);

	if (tag) {
		gdl = netHudRenderTag(gdl, tag);
	}

	if (panel) {
		gdl = netHudRenderPanel(gdl, &top);
	}

	gdl = netHudRenderFeed(gdl, top);
	gdl = text0f153780(gdl);

	return gdl;
}
