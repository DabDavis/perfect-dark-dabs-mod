#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "system.h"
#include "input.h"
#include "lib/joy.h"
#include "lib/collision.h"
#include "game/chraction.h"
#include "game/coop.h"
#include "game/mplayer/mplayer.h"
#include "game/player.h"
#include "game/playermgr.h"
#include "lib/vi.h"
#include "game/prop.h"
#include "game/game_1531a0.h"
#include "net/net.h"
#include "netint.h"

/**
 * A spectator's camera (protocol 9; netproto.h "Join in progress").
 *
 * A spectator has no player in the match: every player is a puppet here,
 * posed from the snapshots, and the picture is drawn through player 0's
 * pass (g_NetLocalSlot 0), whose own tick never runs. In its place this
 * builds the camera each tick and hands it to player0f0c1840, which finds
 * the camera's room the way a player's eye finds its own:
 *
 *   - follow (the default): behind and above the followed player, pulled in
 *     where a wall stands between them, looking past its shoulder the way
 *     it faces; fire (Z) goes on to the next player in the match, A swaps
 *     to the free camera and back;
 *   - free: flown by the stick and the mouse from where the follow camera
 *     was, through walls (func0f065e74 keeps a room list for it as
 *     modspectate.c's does).
 *
 * The camera player's view mode is third person, so neither its gun nor its
 * HUD is drawn. ESC leaves the match. --net-test-spec-cycle N moves on to the
 * next player every N ticks, --net-test-spec-free T flies free from tick T
 * (tools/ci/netjointest.sh).
 *
 * The same two cameras serve a co-op player whose death does not come back
 * (Mission Respawn off, or Mission Lives spent: netCoopRespawnDue(); the
 * user, 2026-10-08: "let the dead players only spectate with no weapons
 * allowed"):
 * netSpecDeadCameraTick() from playerTick's dead camera, on this machine's
 * own player alone. Once the death has faded to black the picture fades back
 * in on a living player, and Z and A work as above; the player stays dead -
 * no gun, nothing to pick up, nobody's target - until the mission ends. When
 * nobody is left alive the camera takes whatever body still stands: the last
 * death's, which GoldenEye's replay poses again (gedeathcam.c).
 */

#define NETSPEC_BACK   170.f
#define NETSPEC_UP     55.f
#define NETSPEC_SPEED  14.f

static s32 s_On = 0;
static s32 s_Target = -1;
static s32 s_Free = 0;
static s32 s_HaveCam = 0;
static struct coord s_CamPos;
static RoomNum s_CamRooms[8];
static f32 s_Yaw = 0;    // radians, the facing chrGetInverseTheta gives
static f32 s_Pitch = 0;
static s32 s_CycleEvery = 0;
static u32 s_FreeAt = 0;
static u32 s_LastCycle = 0;
static u32 s_Switches = 0;
static u32 s_Ticks = 0;
static u32 s_NoTarget = 0;
static u32 s_FreeTicks = 0;
static u32 s_Pulled = 0;
static f32 s_Flown = 0;  // units the free camera has moved
static u32 s_FollowTicks[MAX_PLAYERS];
static s32 s_Dead = 0;        // this machine's player, dead in a co-op mission, watches
static s32 s_DeadLife = 0;    // the death it watches since (lifestarttime60)

static s32 netSpecEligible(s32 pn)
{
	struct player *p;

	if (pn < 0 || pn >= PLAYERCOUNT() || !(p = g_Vars.players[pn]) || !p->prop || !p->prop->chr) {
		return 0;
	}

	// a dead player watches the others
	if (s_Dead && pn == g_NetLocalSlot) {
		return 0;
	}

	// a seat out of play (hidden, dead) is nobody to watch
	return !(p->prop->chr->chrflags & CHRCFLAG_HIDDEN);
}

static void netSpecNext(s32 dir, const char *why)
{
	const s32 n = PLAYERCOUNT();
	s32 pass;
	s32 k;

	// a dead co-op player looks for the living first, then any body still up
	for (pass = s_Dead ? 0 : 1; pass < 2; pass++) {
		for (k = 1; k <= n; k++) {
			const s32 pn = ((s_Target < 0 ? (dir > 0 ? -1 : 0) : s_Target) + dir * k + n * 2) % n;

			if (netSpecEligible(pn) && (pass || !g_Vars.players[pn]->isdead)) {
				if (pn != s_Target) {
					char name[16];

					snprintf(name, sizeof(name), "%s", g_PlayerConfigsArray[g_Vars.playerstats[pn].mpindex].base.name);
					name[strcspn(name, "\n")] = '\0';
					s_Switches++;
					sysLogPrintf(LOG_NOTE, "net: %s (tick %u): following player %d (\"%s\") (%s)",
							s_Dead ? "co-op: out of the mission" : "spectator", g_NetTick, pn, name, why);
				}

				s_Target = pn;
				return;
			}
		}
	}
}

void netSpecStop(void)
{
	s_On = 0;
	s_Dead = 0;
	s_Target = -1;
	s_HaveCam = 0;
}

void netSpecStageStart(void)
{
	netSpecStop();
	s_On = netSessionSpectating();
	s_Free = 0;
	s_CycleEvery = sysArgGetInt("--net-test-spec-cycle", 0);
	s_FreeAt = (u32)sysArgGetInt("--net-test-spec-free", 0);
	s_LastCycle = 0;
	s_Switches = s_Ticks = s_NoTarget = s_FreeTicks = s_Pulled = 0;
	s_Flown = 0;
	memset(s_FollowTicks, 0, sizeof(s_FollowTicks));

	if (s_On) {
		sysLogPrintf(LOG_NOTE, "net: spectator: watching through player %d's pass; Z the next player, A the free camera%s",
				g_NetLocalSlot, s_CycleEvery ? " (--net-test-spec-cycle)" : "");
	}
}

void netSpecLog(const char *why)
{
	char line[128];
	s32 len = 0;
	s32 i;

	if (!s_On && !s_Dead) {
		return;
	}

	line[0] = '\0';

	for (i = 0; i < MAX_PLAYERS; i++) {
		len += snprintf(line + len, sizeof(line) - len, "%s%u", i ? " " : "", s_FollowTicks[i]);
	}

	sysLogPrintf(LOG_NOTE, "net: %s %s (tick %u): %u camera ticks, following per player [%s], free %u (flown %.0f units), nobody to follow %u, switches %u, pulled in by a wall %u",
			s_Dead ? "co-op: out of the mission, watching" : "spectator",
			why, g_NetTick, s_Ticks, line, s_FreeTicks, s_Flown, s_NoTarget, s_Switches, s_Pulled);
}

static void netSpecBasis(const struct coord *look, struct coord *up)
{
	// up square to the look, the world's up as near as it can be
	struct coord right;
	f32 len;

	right.x = -look->z;
	right.y = 0;
	right.z = look->x;
	len = sqrtf(right.x * right.x + right.z * right.z);

	if (len < 0.0001f) {
		up->x = 0;
		up->y = 0;
		up->z = 1;
		return;
	}

	right.x /= len;
	right.z /= len;

	up->x = right.y * look->z - right.z * look->y;
	up->y = right.z * look->x - right.x * look->z;
	up->z = right.x * look->y - right.y * look->x;

	if (up->y < 0) {
		up->x = -up->x;
		up->y = -up->y;
		up->z = -up->z;
	}
}

static void netSpecFollow(void)
{
	struct prop *tprop = g_Vars.players[s_Target]->prop;
	struct coord eye = tprop->pos;
	struct coord cam;
	struct coord aim;
	struct coord look;
	struct coord up;
	const f32 inv = chrGetInverseTheta(tprop->chr);
	const f32 fx = sinf(inv);
	const f32 fz = cosf(inv);
	f32 frac = 1;
	f32 len;
	s32 i;

	cam.x = eye.x - fx * NETSPEC_BACK;
	cam.y = eye.y + NETSPEC_UP;
	cam.z = eye.z - fz * NETSPEC_BACK;

	// a wall between: in, a halving at a time, to where it is clear
	if (!cdTestLos04(&eye, tprop->rooms, &cam, CDTYPE_BG)) {
		s_Pulled += g_NetPass >= NETPASS_TICK;

		for (i = 0; i < 6; i++) {
			struct coord t;

			frac *= 0.6f;
			t.x = eye.x + (cam.x - eye.x) * frac;
			t.y = eye.y + (cam.y - eye.y) * frac;
			t.z = eye.z + (cam.z - eye.z) * frac;

			if (cdTestLos04(&eye, tprop->rooms, &t, CDTYPE_BG)) {
				break;
			}
		}

		cam.x = eye.x + (cam.x - eye.x) * frac;
		cam.y = eye.y + (cam.y - eye.y) * frac;
		cam.z = eye.z + (cam.z - eye.z) * frac;
	}

	aim.x = eye.x + fx * 80.f;
	aim.y = eye.y - 15.f;
	aim.z = eye.z + fz * 80.f;

	look.x = aim.x - cam.x;
	look.y = aim.y - cam.y;
	look.z = aim.z - cam.z;
	len = sqrtf(look.x * look.x + look.y * look.y + look.z * look.z);

	if (len < 1) {
		look.x = fx;
		look.y = 0;
		look.z = fz;
	} else {
		look.x /= len;
		look.y /= len;
		look.z /= len;
	}

	netSpecBasis(&look, &up);
	player0f0c1840(&cam, &up, &look, &tprop->pos, tprop->rooms);
	s_FollowTicks[s_Target] += g_NetPass >= NETPASS_TICK;

	// where the free camera starts from
	s_CamPos = cam;
	memcpy(s_CamRooms, g_Vars.currentplayer->prop->rooms, sizeof(s_CamRooms));
	s_CamRooms[0] = g_Vars.currentplayer->cam_room;
	s_CamRooms[1] = -1;
	s_Yaw = atan2f(look.x, look.z);
	s_Pitch = asinf(look.y < -1 ? -1 : look.y > 1 ? 1 : look.y);
	s_HaveCam = 1;
}

static void netSpecFly(s32 move)
{
	const s32 pad = netPlayersLocalPad();
	const s32 tick = move && g_NetPass >= NETPASS_TICK && !g_NetReplaying; // it moves on ticks; a frame between draws it where it is
	struct coord look;
	struct coord up;
	struct coord dst;
	RoomNum dstrooms[8];
	f32 mdx = 0;
	f32 mdy = 0;
	f32 fwd;
	f32 side;

	if (tick) {
		netMouseDelta(g_NetLocalSlot, &mdx, &mdy);
		s_Yaw -= mdx * 0.01f + joyGetStickX(pad) * 0.0008f;
		s_Pitch -= mdy * 0.01f;
	}

	s_Pitch = s_Pitch < -1.4f ? -1.4f : s_Pitch > 1.4f ? 1.4f : s_Pitch;

	look.x = sinf(s_Yaw) * cosf(s_Pitch);
	look.y = sinf(s_Pitch);
	look.z = cosf(s_Yaw) * cosf(s_Pitch);

	fwd = tick ? joyGetStickY(pad) / 80.f : 0;
	side = !tick ? 0 : (joyGetButtons(pad, R_CBUTTONS) ? 1.f : 0) - (joyGetButtons(pad, L_CBUTTONS) ? 1.f : 0);

	dst.x = s_CamPos.x + (look.x * fwd - look.z * side) * NETSPEC_SPEED;
	dst.y = s_CamPos.y + look.y * fwd * NETSPEC_SPEED;
	dst.z = s_CamPos.z + (look.z * fwd + look.x * side) * NETSPEC_SPEED;

	func0f065e74(&s_CamPos, s_CamRooms, &dst, dstrooms);
	s_Flown += sqrtf((dst.x - s_CamPos.x) * (dst.x - s_CamPos.x) + (dst.y - s_CamPos.y) * (dst.y - s_CamPos.y)
			+ (dst.z - s_CamPos.z) * (dst.z - s_CamPos.z));
	s_CamPos = dst;
	memcpy(s_CamRooms, dstrooms, sizeof(s_CamRooms));

	netSpecBasis(&look, &up);
	player0f0c1840(&s_CamPos, &up, &look, &s_CamPos, s_CamRooms);
	s_FreeTicks += tick;
}

// A and Z on a tick: the free camera, the next player
static void netSpecControls(s32 pad)
{
	if (joyGetButtonsPressedThisFrame(pad, A_BUTTON) || (s_FreeAt && g_NetTick == s_FreeAt)) {
		s_Free = !s_Free;
		sysLogPrintf(LOG_NOTE, "net: %s (tick %u): %s camera at %.0f %.0f %.0f", s_Dead ? "co-op: out of the mission" : "spectator",
				g_NetTick, s_Free ? "free" : "follow", s_CamPos.x, s_CamPos.y, s_CamPos.z);
	}

	if (joyGetButtonsPressedThisFrame(pad, Z_TRIG)) {
		netSpecNext(1, "fire");
	} else if (s_CycleEvery > 0 && g_NetTick - s_LastCycle >= (u32)s_CycleEvery) {
		s_LastCycle = g_NetTick;
		netSpecNext(1, "--net-test-spec-cycle");
	}
}

/**
 * The camera player's tick (netpuppets.c, in place of its lvTickPlayer):
 * the input, the target, the camera
 */
s32 netSpecCameraTick(void)
{
	const s32 pad = netPlayersLocalPad();

	if (!s_On) {
		return 0;
	}

	s_Ticks += g_NetPass >= NETPASS_TICK;
	g_Vars.currentplayer->cameramode = CAMERAMODE_THIRDPERSON;

	// the view as playerTick sets it up for a player of its own: the whole
	// screen (VIEWCOUNT is 1 in a net game), this machine's aspect
	{
		const f32 aspect = player0f0bd358();

		playermgrSetFovY(PLAYER_DEFAULT_FOV);
		playermgrSetAspectRatio(aspect);
		playermgrSetViewSize(playerGetViewportWidth(), playerGetViewportHeight());
		playermgrSetViewPosition(playerGetViewportLeft(), playerGetViewportTop());
		viSetFovAspectAndSize(PLAYER_DEFAULT_FOV, aspect, playerGetViewportWidth(), playerGetViewportHeight());
		viSetViewPosition(playerGetViewportLeft(), playerGetViewportTop());
		viSetSize(playerGetFbWidth(), playerGetFbHeight());
		viSetBufSize(playerGetFbWidth(), playerGetFbHeight());
	}

	if (g_NetPass >= NETPASS_TICK) {
		if (inputKeyJustPressed(VK_ESCAPE)) {
			netClientLeave();
			return 1;
		}

		netSpecControls(pad);
	}

	if (!netSpecEligible(s_Target)) {
		netSpecNext(1, s_Target < 0 ? "the first" : "the last one went out of play");
	}

	if (s_Free && s_HaveCam) {
		netSpecFly(1);
	} else if (netSpecEligible(s_Target)) {
		netSpecFollow();
	} else {
		s_NoTarget += g_NetPass >= NETPASS_TICK;
	}

	return 1;
}

s32 netSpecOn(void)
{
	return s_On;
}

/**
 * playerTick's camera for this machine's own player, dead in an online co-op
 * mission whose death does not come back: 1 when it built the camera (follow
 * or free, as a spectator's), 0 for the game's own death camera - before the
 * death has faded to black, when the new life is on its way, and for the
 * death that lost the mission (GoldenEye's replay, or the end).
 */
s32 netSpecDeadCameraTick(void)
{
	struct player *pl = g_Vars.currentplayer;

	// the host runs every player's pass: only this machine's own is watching
	if (g_NetMode == NETMODE_NONE || netSessionSpectating() || g_Vars.currentplayernum != g_NetLocalSlot) {
		return 0;
	}

	if (g_Vars.normmplayerisrunning || g_Vars.coopplayernum < 0 || !pl->isdead || netCoopRespawnDue(g_Vars.currentplayernum)) {
		s_Dead = 0;
		return 0;
	}

	if (!s_Dead || s_DeadLife != pl->lifestarttime60) {
		// out once the death has gone to black, unless nobody is left alive
		// (the mission is lost, and ends on its own)
		if (!pl->redbloodfinished || !pl->deathanimfinished || !playerIsFadeComplete() || pl->colourscreenfrac < 1
				|| coopAllDead() || netCoopLostSubject() >= 0) {
			s_Dead = 0;
			return 0;
		}

		s_Dead = 1;
		s_DeadLife = pl->lifestarttime60;
		s_Free = 0;
		s_Target = -1;
		s_HaveCam = 0;
		s_Switches = s_Ticks = s_NoTarget = s_FreeTicks = s_Pulled = 0;
		s_Flown = 0;
		memset(s_FollowTicks, 0, sizeof(s_FollowTicks));
		sysLogPrintf(LOG_NOTE, "net: co-op: player %d is out of the mission (tick %u): watching the others, Z the next, A the free camera",
				g_Vars.currentplayernum, g_NetTick);
		netSpecNext(1, "the first");

		// the picture back from the death's black
		playerSetFadeColour(0, 0, 0, 1);
		playerSetFadeFrac(60, 0);
	}

	s_Ticks += g_NetPass >= NETPASS_TICK && !g_NetReplaying;

	if (g_NetPass >= NETPASS_TICK && !g_NetReplaying && !mpIsPaused()) {
		netSpecControls(netPlayersLocalPad());
	}

	if (!netSpecEligible(s_Target)) {
		netSpecNext(1, s_Target < 0 ? "the first" : "the last one went out of play");
	}

	if (s_Free && s_HaveCam) {
		netSpecFly(1);
	} else if (netSpecEligible(s_Target)) {
		netSpecFollow();
	} else if (s_HaveCam) {
		// nobody up to watch: the camera stays where it was
		s_NoTarget += g_NetPass >= NETPASS_TICK && !g_NetReplaying;
		netSpecFly(0);
	} else {
		s_NoTarget += g_NetPass >= NETPASS_TICK && !g_NetReplaying;
		return 0;
	}

	return 1;
}

s32 netSpecDeadOn(void)
{
	return s_Dead && g_Vars.currentplayernum == g_NetLocalSlot && g_Vars.currentplayer->isdead
		&& s_DeadLife == g_Vars.currentplayer->lifestarttime60;
}

/**
 * mpRenderModalText's hook: the camera player is the host's, so its
 * "Press START" (it is dead) means nothing here. A spectator gets who it
 * watches and its keys along the foot of the view instead.
 */
void *netSpecRenderText(void *gdlp)
{
	Gfx *gdl = gdlp;
	char text[96];
	s32 textwidth;
	s32 textheight;
	s32 x;
	s32 y;

	// a dead co-op player's ESC is its pause, as before
	const char *leave = s_Dead ? "" : "   ESC: leave";

	if (s_Free) {
		snprintf(text, sizeof(text), "Free camera   A: follow%s", leave);
	} else if (netSpecEligible(s_Target)) {
		char name[16];

		snprintf(name, sizeof(name), "%s", g_PlayerConfigsArray[g_Vars.playerstats[s_Target].mpindex].base.name);
		name[strcspn(name, "\n")] = '\0';
		snprintf(text, sizeof(text), "Watching %s   Z: next   A: free camera%s", name, leave);
	} else {
		snprintf(text, sizeof(text), "Spectating   A: free camera%s", leave);
	}

	gdl = text0f153628(gdl);
	textMeasure(&textheight, &textwidth, text, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0);
	x = viGetViewLeft() + (viGetViewWidth() - textwidth) / 2;
	y = viGetViewTop() + viGetViewHeight() - textheight - 12;
	gdl = textRender(gdl, &x, &y, text, g_CharsHandelGothicSm, g_FontHandelGothicSm, 0xffffffcc, 0x000000ff, viGetWidth(), viGetHeight(), 0, 0);
	gdl = text0f153780(gdl);

	return gdl;
}
