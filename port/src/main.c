#include <stdlib.h>
#include <stdio.h>
#include <PR/ultratypes.h>
#include <PR/ultrasched.h>
#include <PR/os_message.h>

#include "lib/main.h"
#include "game/modoptions.h"
#include "game/modghost.h"
#include "ghostnet.h"
#include "update.h"
#include "game/modspectate.h"
#include "game/mplayer/mplayer.h"
#include "bss.h"
#include "data.h"

#include "video.h"
#include "../fast3d/gfx_api.h"
#include "audio.h"
#include "input.h"
#include "fs.h"
#include "modloader.h"
#include "romdata.h"
#include "record.h"
#include "screenshot.h"
#include "config.h"
#include "mod.h"
#include "system.h"
#include "utils.h"

u32 g_OsMemSize = 0;
// Upstream's 16 is the N64's 8MB with room to spare. This fork spends memory the
// N64 never had: eighty simulants rather than eight - each one a head modeldef of
// its own, some fifty kilobytes, because a head is offset to fit its body - and
// bodies that stay where they fell. 64MB is nothing on a PC and is what those
// cost with room over; pd.ini wins over this, so a file written by an older build
// still says 16 and will want raising by hand.
s32 g_OsMemSizeMb = 64;
u8 g_Is4Mb = 0;
s8 g_Resetting = false;
OSSched g_Sched;

OSMesgQueue g_MainMesgQueue;
OSMesg g_MainMesgBuf[32];

u8 *g_MempHeap = NULL;
u32 g_MempHeapSize = 0;

u32 g_VmNumTlbMisses = 0;
u32 g_VmNumPageMisses = 0;
u32 g_VmNumPageReplaces = 0;
u8 g_VmShowStats = 0;

s32 g_TickRateDiv = 1;
s32 g_TickExtraSleep = true;

s32 g_SkipIntro = false;

s32 g_FileAutoSelect = -1;

extern s32 g_StageNum;

s32 bootGetMemSize(void)
{
	return (s32)g_OsMemSize;
}

void *bootAllocateStack(s32 threadid, s32 size)
{
	static u8 bruh[0x1000];
	return bruh;
}

void bootCreateSched(void)
{
	osCreateMesgQueue(&g_MainMesgQueue, g_MainMesgBuf, ARRAYCOUNT(g_MainMesgBuf));
	if (osTvType == OS_TV_MPAL) {
		osCreateScheduler(&g_Sched, NULL, OS_VI_MPAL_LAN1, 1);
	} else {
		osCreateScheduler(&g_Sched, NULL, OS_VI_NTSC_LAN1, 1);
	}
}

static void gameInit(void)
{
	osMemSize = g_OsMemSizeMb * 1024 * 1024;

	for (s32 i = 0; i < MAX_PLAYERS; ++i) {
		struct extplayerconfig *cfg = g_PlayerExtCfg + i;
		cfg->fovzoommult = cfg->fovzoom ? cfg->fovy / 60.0f : 1.0f;
	}

	if (g_HudCenter == HUDCENTER_NORMAL) {
		g_HudAlignModeL = G_ASPECT_CENTER_EXT;
		g_HudAlignModeR = G_ASPECT_CENTER_EXT;
	} else if (g_HudCenter == HUDCENTER_WIDE) {
		g_HudAlignModeL = G_ASPECT_LEFT_EXT | G_ASPECT_WIDE_EXT;
		g_HudAlignModeR = G_ASPECT_RIGHT_EXT | G_ASPECT_WIDE_EXT;
	}
}

static void cleanup(void)
{
	sysLogPrintf(LOG_NOTE, "shutdown");
	// Before anything else: an unfinished mp4 has no index and will not play.
	recordStop();
	inputSaveBinds();
	configSave(CONFIG_PATH);
	videoShutdown();
	crashShutdown();
	// TODO: actually shut down all subsystems

	// Waits for the update worker if one is running, which matters: quitting
	// in the middle of a download is fine and leaves a file the next start
	// deletes, but quitting between the two renames that swap the binary would
	// leave the game somewhere it cannot be started from.
	updateShutdown();

	// Last, and only if Check for Updates put a new build in place. It goes
	// here rather than after mainProc() because what starts must not be
	// sharing a window, an audio device or a config file with what it
	// replaces, and this is the point where none of those are open any more.
	// On everything but Windows it never returns.
	updateRelaunchIfStaged();
}

int main(int argc, const char **argv)
{
	sysInitArgs(argc, argv);

	if (!sysArgCheck("--no-crash-handler")) {
		crashInit();
	}

	sysInit();
	fsInit();
	configInit();
	videoInit();
	inputInit();
	screenshotInit();
	recordInit();
	ghostnetInit();
	updateInit();
	audioInit();
	romdataInit();
	modloaderInit();

	g_ValidGbcRomFound = romdataCheckGbcRom();

	gameInit();

	if (fsGetModDir()) {
		modConfigLoad(MOD_CONFIG_FNAME);
	}

	atexit(cleanup);

	bootCreateSched();

	g_OsMemSize = osGetMemSize();

	g_MempHeapSize = g_OsMemSize;
	g_MempHeap = sysMemZeroAlloc(g_MempHeapSize);
	if (!g_MempHeap) {
		sysFatalError("Could not alloc %u bytes for memp heap.", g_MempHeapSize);
	}

	sysLogPrintf(LOG_NOTE, "memp heap at %p - %p", g_MempHeap, g_MempHeap + g_MempHeapSize);
	sysLogPrintf(LOG_NOTE, "rom  file at %p - %p", g_RomFile, g_RomFile + g_RomFileSize);

	g_SndDisabled = sysArgCheck("--no-sound");

	// Renderer cost, printed every N frames. --gfxbatch caps how many triangles
	// may share a draw call, which is what tells a frame that is bound by
	// issuing draw calls apart from one bound by transforming vertices.
	g_GfxLogStats = sysArgGetInt("--gfxstats", 0);
	g_GfxMaxBufferedTris = sysArgGetInt("--gfxbatch", g_GfxMaxBufferedTris);
	g_GfxTexCacheSize = sysArgGetInt("--gfxtexcache", g_GfxTexCacheSize);

	// Spectator from the first frame. A button press cannot happen before the
	// stage loads, and the headless runs that want this cannot press one at all.
	//
	// This is not the Start Spectating setting: that one is saved on exit, and a
	// flag passed once should not tick a box in Dab's Mod Options for good.
	g_ModSpectateStartArg = sysArgCheck("--spectate");
	g_MpEndlessMatch = sysArgCheck("--endless");

	g_StageNum = sysArgGetInt("--boot-stage", STAGE_TITLE);

	if (g_StageNum == STAGE_TITLE && (sysArgCheck("--skip-intro") || g_SkipIntro)) {
		// shorthand for --boot-stage 0x26
		g_StageNum = STAGE_CITRAINING;
	} else if (g_StageNum < 0x01 || g_StageNum > 0x5d) {
		// stage num out of range
		g_StageNum = STAGE_TITLE;
	}

	if (g_StageNum != STAGE_TITLE) {
		sysLogPrintf(LOG_NOTE, "boot stage set to 0x%02x", g_StageNum);
	}

	g_FileAutoSelect = sysArgGetInt("--profile", -1);
	if (g_FileAutoSelect >= 0) {
		sysLogPrintf(LOG_NOTE, "player profile set to %d", g_FileAutoSelect);
	}

	mainProc();

	return 0;
}

PD_CONSTRUCTOR static void gameConfigInit(void)
{
	configRegisterInt("Game.MemorySize", &g_OsMemSizeMb, 4, 2048);
	configRegisterInt("Game.CenterHUD", &g_HudCenter, 0, 2);
	configRegisterInt("Game.MenuMouseControl", &g_MenuMouseControl, 0, 1);
	configRegisterFloat("Game.ScreenShakeIntensity", &g_ViShakeIntensityMult, 0.f, 10.f);
	configRegisterInt("Game.TickRateDivisor", &g_TickRateDiv, 0, 10);
	configRegisterInt("Game.ExtraSleep", &g_TickExtraSleep, 0, 1);
	configRegisterInt("Game.SkipIntro", &g_SkipIntro, 0, 1);
	configRegisterInt("Game.DisableMpDeathMusic", &g_MusicDisableMpDeath, 0, 1);
	configRegisterInt("Game.GEMuzzleFlashes", &g_BgunGeMuzzleFlashes, 0, 1);
	configRegisterInt("Game.MaxExplosions", &g_MaxExplosions, 6, 96);

	// Dab's Mod Options: what this fork added, and how to turn it off. See
	// src/include/game/modoptions.h.
	configRegisterInt("Mod.JumpHeight", &g_ModOptions.jumpheight, 0, JUMPHEIGHT_MAX);
	configRegisterInt("Mod.JumpFor", &g_ModOptions.jumpwho, MODWHO_EVERYONE, MODWHO_PLAYERSONLY);
	configRegisterInt("Mod.CombatRoll", &g_ModOptions.roll, MODROLL_OFF, MODROLL_PLAYERSONLY);
	configRegisterInt("Mod.MeleeCombos", &g_ModOptions.melee, 0, 1);
	configRegisterInt("Mod.FlinchWhenShot", &g_ModOptions.flinch, 0, 1);
	configRegisterInt("Mod.StartArmed", &g_ModOptions.spawnweapon, SPAWNWEAPON_OFF, SPAWNWEAPON_RANDOM);
	configRegisterInt("Mod.StartArmedFor", &g_ModOptions.spawnweaponwho, MODWHO_EVERYONE, MODWHO_PLAYERSONLY);
	configRegisterFloat("Mod.ThirdPersonDistance", &g_ModOptions.camdist, 60.f, 600.f);
	configRegisterFloat("Mod.ThirdPersonClearance", &g_ModOptions.camclearance, 0.f, 120.f);
	configRegisterFloat("Mod.ThirdPersonMinDistance", &g_ModOptions.cammindist, 0.f, 300.f);
	configRegisterInt("Mod.Bodies", &g_ModOptions.bodies, MODBODIES_OFF, MODBODIES_MAX);
	configRegisterInt("Mod.BodyTime", &g_ModOptions.bodytime, MODBODYTIME_OFF, MODBODYTIME_MAX);
	configRegisterInt("Mod.BodiesDrawn", &g_ModOptions.bodiesdrawn, MODBODIESDRAWN_ALL, MODBODIESDRAWN_MAX);
	configRegisterInt("Mod.SpectateStart", &g_ModSpectateStart, 0, 1);
	configRegisterFloat("Mod.SpectateSpeed", &g_ModSpectateSpeed, 1.f, 200.f);
	// Recording is not a mission setting any more - it is what Ghost Trials
	// does - so the lowest this goes is Record Only. A config written when Off
	// was a choice clamps up to it rather than leaving trials recording
	// nothing.
	configRegisterInt("Mod.GhostTimeTrial", &g_ModGhostMode, MODGHOST_RECORD, MODGHOST_RACE);
	// Up to Chosen, not My Best. The range stopped one short of the last pick,
	// so choosing Chosen Ghosts was clamped back to My Best Only the moment
	// the config was read - the menu offered a mode the file could not hold.
	configRegisterInt("Mod.GhostOpponent", &g_ModGhostPick, MODGHOSTPICK_FASTEST, MODGHOSTPICK_CHOSEN);
	configRegisterInt("Mod.GhostVisibility", &g_ModGhostAlpha, 8, 254);
	configRegisterInt("Mod.GhostSplitTimes", &g_ModGhostSplits, 0, 1);
	configRegisterInt("Mod.GhostRacers", &g_ModGhostMaxRacers, 1, MODGHOST_MAXRACERS);

	// The trial character, as a Combat Simulator body index plus one. Zero is
	// Joanna. The ceiling is the table's length rather than one less, because
	// the value stored is the index plus one; anything past it is clamped
	// where it is used, since mpGetBodyId() reads off the end of its array for
	// the value just above the last valid one.
	configRegisterInt("Mod.GhostCharacter", &g_ModGhostBody, 0, 61);
	configRegisterInt("Mod.GhostCharacterHead", &g_ModGhostHead, 0, 255);

	// The leaderboard account. The PIN is stored as typed, which is what a PIN
	// with no password behind it amounts to - it is a claim on a name on a
	// game leaderboard, not a credential worth protecting on disk. It travels
	// over TLS and the server rate limits guesses, which is where the actual
	// protection is.
	configRegisterString("Mod.GhostUser", g_GhostNetUser, GHOSTNET_MAXUSER);
	configRegisterString("Mod.GhostPin", g_GhostNetPin, GHOSTNET_MAXPIN);

	// The accounts this machine remembers besides the active one. Numbered
	// from two so that Mod.GhostUser stays the one in use and a pd.ini written
	// before there was a chooser still signs the same person in.
	for (s32 i = 0; i < GHOSTNET_MAXACCOUNTS - 1; i++) {
		char key[32];

		snprintf(key, sizeof(key), "Mod.GhostUser%d", i + 2);
		configRegisterString(key, g_GhostNetSavedUser[i], GHOSTNET_MAXUSER);

		snprintf(key, sizeof(key), "Mod.GhostPin%d", i + 2);
		configRegisterString(key, g_GhostNetSavedPin[i], GHOSTNET_MAXPIN);

		// Each remembered account keeps its own character, in the same range
		// as the active one above. Without these, switching accounts brought
		// the name back and left whoever the last account was being played as
		// wearing it.
		snprintf(key, sizeof(key), "Mod.GhostCharacter%d", i + 2);
		configRegisterInt(key, &g_GhostNetSavedBody[i], 0, 61);

		snprintf(key, sizeof(key), "Mod.GhostCharacterHead%d", i + 2);
		configRegisterInt(key, &g_GhostNetSavedHead[i], 0, 255);
	}
	configRegisterString("Mod.GhostServer", g_GhostNetUrl, sizeof(g_GhostNetUrl) - 1);
	// Empty for the releases this build's channel points at. See the note in
	// update.c for what setting it means.
	configRegisterString("Mod.UpdateServer", g_UpdateUrl, sizeof(g_UpdateUrl) - 1);

	for (s32 j = 0; j < MAX_PLAYERS; ++j) {
		const s32 i = j + 1;
		configRegisterFloat(strFmt("Game.Player%d.FovY", i), &g_PlayerExtCfg[j].fovy, 5.f, 175.f);
		configRegisterInt(strFmt("Game.Player%d.FovAffectsZoom", i), &g_PlayerExtCfg[j].fovzoom, 0, 1);
		configRegisterInt(strFmt("Game.Player%d.MouseAimMode", i), &g_PlayerExtCfg[j].mouseaimmode, 0, 1);
		configRegisterFloat(strFmt("Game.Player%d.MouseAimSpeedX", i), &g_PlayerExtCfg[j].mouseaimspeedx, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.MouseAimSpeedY", i), &g_PlayerExtCfg[j].mouseaimspeedy, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.RadialMenuSpeed", i), &g_PlayerExtCfg[j].radialmenuspeed, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.CrosshairSway", i), &g_PlayerExtCfg[j].crosshairsway, 0.f, 10.f);
		configRegisterFloat(strFmt("Game.Player%d.CrosshairEdgeBoundary", i), &g_PlayerExtCfg[j].crosshairedgeboundary, 0.0f, 1.0f);
		configRegisterInt(strFmt("Game.Player%d.CrouchMode", i), &g_PlayerExtCfg[j].crouchmode, 0, CROUCHMODE_TOGGLE_ANALOG);
		configRegisterInt(strFmt("Game.Player%d.ExtendedControls", i), &g_PlayerExtCfg[j].extcontrols, 0, 1);
		configRegisterUInt(strFmt("Game.Player%d.CrosshairColour", i), &g_PlayerExtCfg[j].crosshaircolour, 0, 0xFFFFFFFF);
		configRegisterUInt(strFmt("Game.Player%d.CrosshairSize", i), &g_PlayerExtCfg[j].crosshairsize, 0, 4);
		configRegisterInt(strFmt("Game.Player%d.CrosshairHealth", i), &g_PlayerExtCfg[j].crosshairhealth, 0, CROSSHAIR_HEALTH_ON_WHITE);
		configRegisterInt(strFmt("Game.Player%d.UseKeyReloads", i), &g_PlayerExtCfg[j].usereloads, 0, false);
	}
}
