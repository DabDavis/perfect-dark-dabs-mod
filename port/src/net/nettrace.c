#include <stdio.h>
#include <string.h>
#include <PR/ultratypes.h>
#include <ultra64.h>
#include "constants.h"
#include "types.h"
#include "bss.h"
#include "data.h"
#include "config.h"
#include "system.h"
#include "geguns.h"
#include "geslappers.h"
#include "game/playermgr.h"
#include "net/net.h"
#include "netint.h"
#include "netrdv.h"

/**
 * F3's [netplay] section (trace.c): what an online report needs that a stock
 * trace never had. The trace is one machine's picture of a match that runs
 * on several, so it says which machine it is and how it reaches the others,
 * the rules in force here and each player's own settings as the host plays
 * it by them, every player's hands (a host ticks them all: the GoldenEye
 * slap's swing kept one pair of slots for every player and ran at twice the
 * speed with one other player in the match, F3-worthy only with this), and
 * how many ticks each frame ran.
 *
 * Every module's running summary (commands, snapshots, events, prediction,
 * lag compensation, puppets) goes into the log "at F3" as well, so the log
 * tail the trace ends with (crashReportWriteContext) carries them.
 */

// the ticks each of the last frames ran: 0 is a frame drawn between ticks
#define NETTRACE_FRAMES 600

static u8 s_FrameTicks[NETTRACE_FRAMES];
static u64 s_FrameUs[NETTRACE_FRAMES];
static u32 s_FrameCount;

void netTraceNoteFrame(s32 ticks)
{
	s_FrameTicks[s_FrameCount % NETTRACE_FRAMES] = ticks < 0 ? 0 : ticks > 255 ? 255 : ticks;
	s_FrameUs[s_FrameCount % NETTRACE_FRAMES] = sysGetMicroseconds();
	s_FrameCount++;
}

static void netTracePacing(FILE *f)
{
	const u32 n = s_FrameCount < NETTRACE_FRAMES ? s_FrameCount : NETTRACE_FRAMES;
	u32 hist[5] = { 0 };
	u32 ticks = 0;
	u32 i;

	for (i = 0; i < n; i++) {
		const u32 t = s_FrameTicks[(s_FrameCount - 1 - i) % NETTRACE_FRAMES];

		hist[t < 4 ? t : 4]++;
		ticks += t;
	}

	{
		const u64 span = n ? s_FrameUs[(s_FrameCount - 1) % NETTRACE_FRAMES] - s_FrameUs[(s_FrameCount - n) % NETTRACE_FRAMES] : 0;
		const f32 secs = span / 1000000.f;

		fprintf(f, "pacing, the last %u frames (%.2f s, %.0f fps, %.1f ticks a second): frames with 0 ticks (drawn between) %u, 1 %u, 2 %u, 3 %u, 4+ %u\n",
				n, secs, secs > 0 ? n / secs : 0.f, secs > 0 ? ticks / secs : 0.f, hist[0], hist[1], hist[2], hist[3], hist[4]);
	}
}

static void netTraceHands(FILE *f, s32 pn, const struct player *p)
{
	s32 h;

	for (h = 0; h < 2; h++) {
		const struct hand *hand = &p->hands[h];
		f32 slaptime = 0, slashtime = 0, throwtime = 0;
		s32 slash, throwstep;
		const s32 slap = geslappersTraceHand(pn, h, &slaptime);

		gegunsOwnSwingTrace(pn, h, &slash, &slashtime, &throwstep, &throwtime);

		if (!hand->inuse && slap < 0 && slash < 0 && throwstep == 0) {
			continue;
		}

		fprintf(f, "    hand %d: weapon 0x%02x, state %d.%d for %d frames (%d cycles), anim %d, trigger %d released %d, firing %d",
				h, hand->gset.weaponnum, hand->state, hand->stateminor, hand->stateframes, hand->statecycles,
				hand->animmode, hand->triggeron, hand->triggerreleased, hand->firing);

		if (slap >= 0) {
			fprintf(f, "; GoldenEye slap track %d at %.1f", slap, slaptime);
		}

		if (slash >= 0) {
			fprintf(f, "; GoldenEye knife slash %d at %.1f", slash, slashtime);
		}

		if (throwstep != 0) {
			fprintf(f, "; GoldenEye throw step %d at %.1f", throwstep, throwtime);
		}

		fprintf(f, "\n");
	}
}

static void netTracePlayers(FILE *f)
{
	s32 pn;

	for (pn = 0; pn < PLAYERCOUNT(); pn++) {
		const struct player *p = g_Vars.players[pn];
		const s32 mpindex = g_Vars.playerstats[pn].mpindex;
		struct netseatinfo seat;
		const char *who;

		if (!p || !p->prop) {
			fprintf(f, "  player %d: no prop\n", pn);
			continue;
		}

		memset(&seat, 0, sizeof(seat));
		netSessionSeatInfo(mpindex % MAX_PLAYERS, &seat);

		who = netIsLocalSlot(pn) ? "this machine's"
			: g_NetMode == NETMODE_SERVER ? "a client's, simulated here"
			: "a puppet, the host's word";

		fprintf(f, "  player %d (mpindex %d) \"%s\", %s: ping %d ms, %s, health %.2f, pos %.0f %.0f %.0f theta %.1f, camera mode %d%s,"
				" weapon 0x%02x (switching to %d)%s%s\n",
				pn, mpindex, seat.name, who, seat.ping, p->isdead ? "dead" : "alive", p->bondhealth,
				p->prop->pos.x, p->prop->pos.y, p->prop->pos.z, p->vv_theta, p->cameramode,
				p->thirdperson ? " third person" : "", p->gunctrl.weaponnum, p->gunctrl.switchtoweaponnum,
				p->invincible ? ", invincible" : "", p->activemenumode ? ", a menu up" : "");

		netTraceHands(f, pn, p);
		netPlayersTraceSlot(f, pn);
	}
}

// pd.ini's [Net], but for the room's secret
static void netTraceSettings(FILE *f)
{
	static const char *keys[] = {
		"Net.Name", "Net.LobbyServer", "Net.Port", "Net.MaxPlayers", "Net.JoinInProgress", "Net.ReconnectHold",
		"Net.LagComp", "Net.LagCompMaxMs", "Net.PlayerNames", "Net.HostRenderAtTickRate",
	};
	struct netkeyvalue kv;
	char buf[NET_MAXSTRVAL + 4];
	u32 i;

	fprintf(f, "pd.ini [Net]:");

	for (i = 0; i < ARRAYCOUNT(keys); i++) {
		if (netRulesReadKey(keys[i], &kv)) {
			netRulesValueString(&kv, buf, sizeof(buf));
			fprintf(f, " %s=%s", keys[i] + 4, buf);
		}
	}

	fprintf(f, "\n");
}

void netTraceWrite(FILE *f)
{
	const char *role = g_NetMode == NETMODE_SERVER ? (g_NetDedicated ? "dedicated host" : "host")
		: g_NetMode == NETMODE_CLIENT ? (netSessionSpectating() ? "client, spectating" : "client") : "offline";
	char addr[64] = "";
	u16 port = 0;

	fprintf(f, "\n[netplay]\n");
	fprintf(f, "%s, protocol %d, tick %u, local slot %d, match %s, the HUD %s\n",
			role, NET_PROTOCOL_VERSION, g_NetTick, g_NetLocalSlot,
			netSessionMatchActive() ? "running" : "not running", netSessionHudLive() ? "live" : "not live");

	if (g_NetMode == NETMODE_CLIENT && !netRdvHasLobby()) {
		fprintf(f, "host \"%s\", connected to directly (no lobby room)\n", netSessionHostTitle());
	} else if (g_NetMode == NETMODE_CLIENT) {
		const s32 path = netRdvPath();

		if (netRdvEndpoint(addr, sizeof(addr), &port) != 0) {
			snprintf(addr, sizeof(addr), "-");
		}

		fprintf(f, "host \"%s\", reached by %s (%s port %u), ping over it %d ms, the lobby %d ms\n",
				netSessionHostTitle(), netRdvPathName(path), addr, port, netRdvPing(), netRdvLobbyRtt());
	}

	netSessionTraceLinks(f);
	netTracePacing(f);
	netRulesTraceSync(f);
	netRulesTraceOwnHere(f);
	netTraceSettings(f);

	if (netSessionMatchActive() && STAGE_IS_LEVEL(g_Vars.stagenum)) {
		fprintf(f, "players (%d):\n", PLAYERCOUNT());
		netTracePlayers(f);
	}

	// every module's summary into the log, which the trace ends with
	netPlayersLogNow("at F3");
	netEntsLogNow("at F3");
	netEventsLogNow("at F3");

	if (g_NetMode == NETMODE_CLIENT) {
		netPredictLog("at F3");
		netPuppetsLog("at F3");
	} else if (g_NetMode == NETMODE_SERVER) {
		netLagCompLog("at F3");
	}

	fprintf(f, "(the commands, snapshots, events%s summaries are in the log below, \"at F3\")\n",
			g_NetMode == NETMODE_CLIENT ? ", prediction and puppets'" : " and lag compensation's");
}
