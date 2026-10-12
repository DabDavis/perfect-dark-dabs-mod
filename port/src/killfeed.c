/**
 * The kill feed (Mod.KillFeed): "Killer [Weapon] Victim" at the top right of
 * a Combat Simulator or GoldenEye match, offline and online.
 *
 * Where a kill is known: mpstatsRecordDeath() is the one place every death in
 * a match is counted - a player's (playerDieByShooter()), a simulant's
 * (chrDie()) and a chr's - with the attacker and the victim as mpchr player
 * numbers. It does not know the weapon, so chrDamage() notes the weapon of
 * the latest hit on each mpchr (killfeedNoteHit()), which for a kill is the
 * killing one; a fall after a hit is counted for the shooter by the game and
 * shown as the game counts it. An explosion's damage has no gset: the
 * explosion keeps what made it (a rocket, a grenade, a mine:
 * killfeedNoteExplosion()), and a projectile is named by its launcher -
 * the one the attacker holds when it has a GoldenEye gun on that host
 * (a GoldenEye rocket launcher fires Perfect Dark's rocket).
 *
 * Online the host decides every death and sends it to everyone as DEATH
 * (netevents.c, reliable and ordered), which now carries the weapon
 * (protocol 31); a client's mpstatsRecordDeath() replay of it takes the
 * weapon from killfeedSetWireWeapon(). Every client so lists every kill.
 *
 * Drawing only: nothing the game reads is written here, and nothing here
 * draws a random number, so an offline replay plays the same game with the
 * feed on or off. Time is the level's 60ths (paused with the level).
 */
#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include "platform.h"
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "system.h"
#include "lib/vi.h"
#include "game/bondgun.h"
#include "game/game_1531a0.h"
#include "game/modoptions.h"
#include "game/mplayer/mplayer.h"
#include "game/playermgr.h"
#include "net/net.h"
#include "gehud.h"
#include "killfeed.h"

#define FEED_LINES      5     // kept and shown in a full view
#define FEED_LINES_SPLIT 3    // in a split screen's view
#define FEED_LIFE       300   // 60ths a line stays (5 s)
#define FEED_FADE       60    // of which the last fade out
#define FEED_MAXNAME    16
#define FEED_MAXWEAPON  40

// DEATH carries a weapon number in a byte, two values above kept
_Static_assert(NUM_WEAPONS < KILLFEED_WEAPON_EXPLOSION, "a weapon number past the kill feed's byte");

#define COL_NAME        0xf0f0f0ff
#define COL_OWNNAME     0xffe070ff
#define COL_WEAPON      0xb8b8b8ff
#define COL_BOX         0x00000070
#define COL_OWNBOX      0x5a5a5ac0

struct feedline {
	s32 at;       // g_Vars.lvframe60
	s8 a, v;      // mpchr player numbers (a -1: "died")
	s8 ateam, vteam;
	char killer[FEED_MAXNAME];
	char victim[FEED_MAXNAME];
	char weapon[FEED_MAXWEAPON];
};

static struct feedline s_Lines[FEED_LINES];
static s32 s_Count = 0;    // lines kept, oldest first
static s32 s_Stage = -1;   // the stage they were kept on

static u8 s_LastHit[MAX_MPCHRS];        // the weapon of each mpchr's latest hit
static u8 s_ExpWeapon[128];             // the weapon each explosion was made by
static s32 s_CurExplosion = -1;         // the explosion dealing damage now
static s32 s_WireWeapon = -1;           // a client's: the weapon DEATH carried
static s32 s_Log = -1;                  // --killfeed-log: every line to the log

s32 killfeedIsOn(void)
{
	const s32 mode = g_ModOptions.killfeed;

	if (mode == KILLFEED_AUTO) {
		return g_NetMode != NETMODE_NONE;
	}

	return mode != KILLFEED_OFF;
}

/** A projectile by what fires it */
static s32 killfeedLauncher(s32 weaponnum)
{
	switch (weaponnum) {
	case WEAPON_ROCKET:
	case WEAPON_HOMINGROCKET: return WEAPON_ROCKETLAUNCHER;
	case WEAPON_GRENADEROUND: return WEAPON_DEVASTATOR;
	case WEAPON_SKROCKET:     return WEAPON_SLAYER;
	case WEAPON_BOLT:         return WEAPON_CROSSBOW;
	}

	return weaponnum;
}

/** What the attacker holds, or -1 */
static s32 killfeedHeld(struct prop *aprop)
{
	if (!aprop) {
		return -1;
	}

	if (aprop->type == PROPTYPE_PLAYER) {
		const s32 pn = playermgrGetPlayerNumByProp(aprop);

		if (pn >= 0 && pn < MAX_PLAYERS && g_Vars.players[pn]) {
			return g_Vars.players[pn]->gunctrl.weaponnum;
		}
	} else if (aprop->type == PROPTYPE_CHR && aprop->chr && aprop->chr->aibot) {
		return aprop->chr->aibot->weaponnum;
	}

	return -1;
}

void killfeedNoteHit(struct chrdata *chr, struct gset *gset, struct prop *aprop, s32 explosion)
{
	s32 index;
	s32 weapon = 0;
	s32 held;

	if (!g_Vars.normmplayerisrunning) {
		return;
	}

	index = mpPlayerGetIndex(chr);

	if (index < 0 || index >= MAX_MPCHRS) {
		return;
	}

	if (gset) {
		weapon = gset->weaponnum;
	}

	if (explosion && weapon <= WEAPON_UNARMED) {
		weapon = s_CurExplosion >= 0 && s_CurExplosion < (s32)ARRAYCOUNT(s_ExpWeapon)
			? s_ExpWeapon[s_CurExplosion] : KILLFEED_WEAPON_EXPLOSION;
	}

	if (weapon <= WEAPON_NONE || (weapon >= NUM_WEAPONS && weapon != KILLFEED_WEAPON_EXPLOSION)) {
		s_LastHit[index] = KILLFEED_WEAPON_NONE;
		return;
	}

	if (weapon < NUM_WEAPONS) {
		weapon = killfeedLauncher(weapon);

		// a GoldenEye gun run by this host's engine is named as itself
		held = killfeedHeld(aprop);

		if (held > WEAPON_UNARMED && held < NUM_WEAPONS && held != weapon && weaponHost(held) == weapon) {
			weapon = held;
		}
	}

	s_LastHit[index] = (u8)weapon;
}

void killfeedNoteExplosion(s32 index, struct prop *source)
{
	u8 weapon = KILLFEED_WEAPON_EXPLOSION;

	if (index < 0 || index >= (s32)ARRAYCOUNT(s_ExpWeapon)) {
		return;
	}

	if (source && source->type == PROPTYPE_WEAPON && source->weapon
			&& source->weapon->weaponnum > WEAPON_UNARMED && source->weapon->weaponnum < NUM_WEAPONS) {
		weapon = source->weapon->weaponnum;
	}

	s_ExpWeapon[index] = weapon;
}

void killfeedSetExplosion(s32 index)
{
	s_CurExplosion = index;
}

u8 killfeedDeathWeapon(s32 aplayernum, s32 vplayernum)
{
	if (aplayernum < 0 || vplayernum < 0 || aplayernum == vplayernum || vplayernum >= MAX_MPCHRS) {
		return KILLFEED_WEAPON_NONE;
	}

	return s_LastHit[vplayernum];
}

void killfeedSetWireWeapon(s32 weapon)
{
	s_WireWeapon = weapon;
}

static void killfeedName(char *out, s32 playernum)
{
	char *p;

	out[0] = '\0';

	if (playernum >= 0 && playernum < g_MpNumChrs && g_MpAllChrConfigPtrs[playernum]) {
		snprintf(out, FEED_MAXNAME, "%.14s", g_MpAllChrConfigPtrs[playernum]->name);
	}

	for (p = out; *p; p++) {
		if (*p == '\n' || *p == '\r') {
			*p = '\0';
			break;
		}
	}

	// a match booted past the setup menus (--mpsims) names nobody
	if (out[0] == '\0') {
		if (playernum >= 0 && playernum < PLAYERCOUNT()) {
			snprintf(out, FEED_MAXNAME, "Player %d", playernum + 1);
		} else {
			snprintf(out, FEED_MAXNAME, "Sim %d", playernum - PLAYERCOUNT() + 1);
		}
	}
}

static s32 killfeedTeam(s32 playernum)
{
	if (!(g_MpSetup.options & MPOPTION_TEAMSENABLED)
			|| playernum < 0 || playernum >= g_MpNumChrs || !g_MpAllChrConfigPtrs[playernum]) {
		return -1;
	}

	return g_MpAllChrConfigPtrs[playernum]->team & 7;
}

void killfeedRecord(s32 aplayernum, s32 vplayernum)
{
	struct feedline *line;
	s32 weapon = KILLFEED_WEAPON_NONE;
	char *p;

	if (s_WireWeapon >= 0) {
		weapon = s_WireWeapon;
		s_WireWeapon = -1;
	} else {
		weapon = killfeedDeathWeapon(aplayernum, vplayernum);
	}

	if (!g_Vars.normmplayerisrunning || vplayernum < 0 || vplayernum >= g_MpNumChrs) {
		return;
	}

	if (s_Stage != g_Vars.stagenum) {
		s_Stage = g_Vars.stagenum;
		s_Count = 0;
	}

	if (s_Count == FEED_LINES) {
		memmove(&s_Lines[0], &s_Lines[1], sizeof(s_Lines[0]) * (FEED_LINES - 1));
		s_Count--;
	}

	line = &s_Lines[s_Count++];
	memset(line, 0, sizeof(*line));

	line->at = g_Vars.lvframe60;
	line->v = vplayernum;
	line->vteam = killfeedTeam(vplayernum);
	killfeedName(line->victim, vplayernum);

	// a fall, a self-kill or the level's own doing: "X died"
	if (aplayernum < 0 || aplayernum == vplayernum || aplayernum >= g_MpNumChrs) {
		line->a = -1;
		line->ateam = -1;
	} else {
		line->a = aplayernum;
		line->ateam = killfeedTeam(aplayernum);
		killfeedName(line->killer, aplayernum);

		if (weapon == KILLFEED_WEAPON_EXPLOSION) {
			snprintf(line->weapon, sizeof(line->weapon), "%s", "Explosion");
		} else if (weapon > WEAPON_NONE && weapon < NUM_WEAPONS) {
			snprintf(line->weapon, sizeof(line->weapon), "%s", bgunGetName(weapon));
		}

		for (p = line->weapon; *p; p++) {
			if (*p == '\n' || *p == '\r') {
				*p = '\0';
				break;
			}
		}
	}

	if (s_Log < 0) {
		s_Log = sysArgCheck("--killfeed-log");
	}

	if (s_Log) {
		if (line->a < 0) {
			sysLogPrintf(LOG_NOTE, "killfeed: %s died (victim %d, frame %d)", line->victim, line->v, g_Vars.lvframenum);
		} else {
			sysLogPrintf(LOG_NOTE, "killfeed: %s [%s] %s (attacker %d victim %d weapon %d, frame %d)",
					line->killer, line->weapon, line->victim, line->a, line->v, weapon, g_Vars.lvframenum);
		}
	}
}

/** A team's colour, lightened to read on the box (radar.c's are pure) */
static u32 killfeedTeamColour(s32 team)
{
	const u32 c = g_TeamColours[team & 7];
	u32 r = (c >> 24) & 0xff, g = (c >> 16) & 0xff, b = (c >> 8) & 0xff;

	r += (0xff - r) / 2;
	g += (0xff - g) / 2;
	b += (0xff - b) / 2;

	return r << 24 | g << 16 | b << 8 | 0xff;
}

static u32 killfeedFade(u32 colour, s32 alpha)
{
	return (colour & 0xffffff00) | (((colour & 0xff) * alpha / 255) & 0xff);
}

// one line's parts: up to four pieces of text, each with its colour
struct feedparts {
	const char *text[4];
	u32 colour[4];
	s32 n;
	u32 box;
};

static void killfeedParts(const struct feedline *line, s32 own, s32 alpha, struct feedparts *parts,
		char *weapon, s32 weaponsize)
{
	const u32 kcol = line->ateam >= 0 ? killfeedTeamColour(line->ateam) : line->a == own ? COL_OWNNAME : COL_NAME;
	const u32 vcol = line->vteam >= 0 ? killfeedTeamColour(line->vteam) : line->v == own ? COL_OWNNAME : COL_NAME;

	parts->n = 0;
	parts->box = killfeedFade((line->a == own || line->v == own) && own >= 0 ? COL_OWNBOX : COL_BOX, alpha);

	if (line->a < 0) {
		parts->text[parts->n] = line->victim;
		parts->colour[parts->n++] = killfeedFade(vcol, alpha);
		parts->text[parts->n] = " died";
		parts->colour[parts->n++] = killfeedFade(COL_WEAPON, alpha);
		return;
	}

	if (line->weapon[0]) {
		snprintf(weapon, weaponsize, " [%s] ", line->weapon);
	} else {
		snprintf(weapon, weaponsize, "%s", " killed ");
	}

	parts->text[parts->n] = line->killer;
	parts->colour[parts->n++] = killfeedFade(kcol, alpha);
	parts->text[parts->n] = weapon;
	parts->colour[parts->n++] = killfeedFade(COL_WEAPON, alpha);
	parts->text[parts->n] = line->victim;
	parts->colour[parts->n++] = killfeedFade(vcol, alpha);
}

/** Perfect Dark's HUD font, laid out like the other HUD text (hudmsg.c) */
static Gfx *killfeedRenderPd(Gfx *gdl, const struct feedparts *parts, s32 n, s32 top)
{
	const s32 prevscalex = g_ScaleX;
	const s32 fullwidth = viGetViewWidth() >= viGetWidth() - 2;
	s32 right;
	s32 y = top;
	s32 height;
	s32 dummy;
	s32 i, j;

	g_ScaleX = g_ViRes == VIRES_HI ? 2 : 1;
	right = (viGetViewLeft() + viGetViewWidth()) / g_ScaleX - 8;

	// a full-width view keeps the HUD's right edge (HUD Centering)
	if (fullwidth) {
		gSPExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT, g_HudAlignModeR);
	}

	gdl = text0f153628(gdl);

	// a line's height: textMeasure() takes it from a newline
	textMeasure(&height, &dummy, "Ay\n", g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);

	for (i = 0; i < n; i++) {
		s32 widths[4];
		s32 width = 0;
		s32 x;

		for (j = 0; j < parts[i].n; j++) {
			s32 h, w;

			textMeasure(&h, &w, (char *)parts[i].text[j], g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);
			widths[j] = w;
			width += w;
			(void)h;
		}

		gdl = textSetPrimColour(gdl, parts[i].box);
		gDPFillRectangleScaled(gdl++, right - width - 4, y - 1, right + 3, y + height + 2);
		gdl = text0f153838(gdl);

		x = right - width;

		for (j = 0; j < parts[i].n; j++) {
			s32 tx = x;
			s32 ty = y;

			gdl = textRender(gdl, &tx, &ty, (char *)parts[i].text[j], g_CharsHandelGothicSm, g_FontHandelGothicSm,
					parts[i].colour[j], parts[i].colour[j] & 0xff, viGetWidth(), viGetHeight(), 0, 0);
			x += widths[j];
		}

		y += height + 4;
	}

	gdl = text0f153780(gdl);

	if (fullwidth) {
		gSPClearExtraGeometryModeEXT(gdl++, G_ASPECT_MODE_EXT);
	}

	g_ScaleX = prevscalex;

	return gdl;
}

Gfx *killfeedRender(Gfx *gdl)
{
	struct feedparts parts[FEED_LINES];
	char weapons[FEED_LINES][FEED_MAXWEAPON + 4];
	const s32 now = g_Vars.lvframe60;
	const s32 max = viGetViewHeight() >= viGetHeight() * 3 / 4 ? FEED_LINES : FEED_LINES_SPLIT;
	const s32 own = g_Vars.currentplayernum;
	s32 n = 0;
	s32 i;

	if (!g_Vars.normmplayerisrunning || s_Count == 0 || s_Stage != g_Vars.stagenum || !killfeedIsOn()) {
		return gdl;
	}

	// online: the players panel (hold P) has the scores up where this goes
	if (g_NetMode != NETMODE_NONE && netHudPanelUp()) {
		return gdl;
	}

	// the newest `max` still up, oldest first
	for (i = s_Count - 1; i >= 0 && s_Count - i <= max; i--) {
		if (now - s_Lines[i].at >= FEED_LIFE || now < s_Lines[i].at) {
			break;
		}
	}

	for (i++; i < s_Count; i++) {
		const s32 age = now - s_Lines[i].at;
		const s32 alpha = age > FEED_LIFE - FEED_FADE ? (FEED_LIFE - age) * 255 / FEED_FADE : 255;

		killfeedParts(&s_Lines[i], own, alpha, &parts[n], weapons[n], sizeof(weapons[n]));
		n++;
	}

	if (n == 0) {
		return gdl;
	}

	if (geHudActive()) {
		struct gehudfeedline lines[FEED_LINES];

		for (i = 0; i < n; i++) {
			s32 j;

			for (j = 0; j < parts[i].n; j++) {
				lines[i].text[j] = parts[i].text[j];
				lines[i].colour[j] = parts[i].colour[j];
			}

			lines[i].n = parts[i].n;
			lines[i].box = parts[i].box;
		}

		return geHudRenderFeed(gdl, lines, n, (g_MpSetup.options & MPOPTION_NORADAR) != 0);
	}

	// under the radar (its disc to 42 rows down), or at the top with none
	return killfeedRenderPd(gdl, parts, n,
			viGetViewTop() + ((g_MpSetup.options & MPOPTION_NORADAR) ? 10 : 48));
}
