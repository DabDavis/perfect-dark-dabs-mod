#ifndef _IN_GEBEAN_H
#define _IN_GEBEAN_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

struct model;
struct modeldef;
struct modelnode;

/**
 * GoldenEye 007 for the Xbox 360 - Rare's 2007 "Project Bean" build - as a
 * source of characters for GoldenEye X.
 *
 * The player drops their copy in xbla/ beside the Perfect Dark release: the
 * archive it came as, or the folder it unpacks to (the one holding default.xex
 * and files/). An archive's characters and heads, and nothing else of it, are
 * unpacked once into cache/xbla/goldeneye/.
 *
 * GoldenEye X keeps GoldenEye's N64 models under Perfect Dark's file names, so
 * a model is paired with Bean's by a table made offline (gebeantable.h, from
 * tools/geconvert/fit/gen_beantable.py): the file's name, and what its list
 * nodes and vertices add up to, so a different mod's file of the same name is
 * left alone. What the pairing gives back is a mesh in 4J's own layout, which
 * xblamesh.c draws like one of the Perfect Dark release's: Bean's vertices
 * re-posed onto the model's rest skeleton and skinned to the model's own
 * matrices, a group per list node. CLAUDE-notes/ge-bean.md has the formats and
 * the reasons.
 *
 * On whenever the release is there (gebeanGetEnabled()): it was a checkbox,
 * Mod.XblaGoldenEye, until 2026-09-21.
 */

/**
 * g_HeadsAndBodies past the stock table, whose terminator is row 151 - the last
 * a mod's own table can import (moddata.c) - so everything from 152 is ours.
 *
 * A converted mission's bodies come first and have rows **kept for them**
 * (gexplus.c): a body's row has to fit the byte a packedchr and aiSpawnChrAtPad
 * hold it in, and the Combat Simulator's pool below - GoldenEye X's borrowed
 * characters are 106 rows of it - used to start at 152 too and left a mission
 * none under 256. Its spawn commands then kept GoldenEye's own numbers, and
 * Egyptian's Baron Samedi, who is GoldenEye's 12, spawned as Perfect Dark's 12:
 * Joanna's head, worn as a body, with no root matrix for a shot to be tested
 * against (chrTestHit()).
 *
 * The pool's rows are never written into a byte - a Combat Simulator body is
 * g_MpBodies[].bodynum, an s16 - so it is the one that moves up.
 */
#define GEROM_BODY_FIRST  152
#define GEROM_BODY_ROWS   24    // GEROM_MAX_ROWS: every row a mission may take
#define GEROM_BODY_LAST   (GEROM_BODY_FIRST + GEROM_BODY_ROWS - 1)
// 4J's Agent 4, whom the release adds and the ROM never had (xblaagent4.c):
// its head's row and its body's, between the mission's rows and the pool
#define XBLA_AGENT4_HEADROW (GEROM_BODY_LAST + 1)
#define XBLA_AGENT4_BODYROW (GEROM_BODY_LAST + 2)
#define GEBEAN_POOL_BASE  (GEROM_BODY_LAST + 3)

/** Whether there is GoldenEye content to draw on: the release, or GoldenEye X to borrow from. */
s32 gebeanGetEnabled(void);

/** Whether a copy was found in xbla/, unpacked or not. Never unpacks. */
s32 gebeanIsAvailable(void);

/**
 * The table row for a model file as it loads, or -1: a mod's file (never a
 * stock one) whose name and shape are in the table.
 */
s32 gebeanFindRow(u16 fileid, struct modeldef *modeldef);

/** The row's file name, for the log. */
const char *gebeanRowName(s32 row);

/**
 * Whether the row is one of the Combat Simulator pool's (gebeanPoolRefresh())
 * rather than GoldenEye X's. A pool row stands on a Perfect Dark model, so
 * with the release's meshes off (F6) it takes Bean's N64-look original; a
 * GoldenEye X model is GoldenEye's N64 one already and draws itself.
 */
s32 gebeanRowIsPool(s32 row);

/**
 * The file the casing a gun ejects is loaded from: for one of GoldenEye's guns,
 * an alias of the casing (bondgun.c's g_CartFileNums[casing]) the release's
 * own spent cartridge is drawn on in the HD look; else `stock`.
 */
u16 gebeanCasingFile(s32 weaponnum, s32 casing, u16 stock);
s32 gebeanRowIsFemale(s32 row);

/**
 * Whether the row is a GoldenEye gun's first-person model. Its mesh is built
 * in each list node's own space, one group a node with no palette, so it is
 * drawn the way a model pack's is - under the node's own matrix - rather than
 * posed like a character.
 */
s32 gebeanRowIsFirstPerson(s32 row);

/**
 * Whether the row is one of the GoldenEye remake's props (propRows): a
 * converted GoldenEye prop model with Bean's HD prop drawn on it rigid.
 */
s32 gebeanRowIsProp(s32 row);

/**
 * Whether the row is a character - a body or a head - rather than a prop or a
 * gun, which are rigid (gebeanBuildRigid()) and may be single planes.
 */
s32 gebeanRowIsChr(s32 row);

/**
 * Whether a body row keeps the hood of the head row cut off its own neck (the
 * parka's; gebeanmats.hood), so that the head leaves it out there.
 */
s32 gebeanRowKeepsHood(s32 bodyrow, s32 headrow);

/**
 * Where the first-person gun drawn for this weapon ends, as an offset from its
 * host's MODELPART_GUN_MUZZLEPOS node in the model's own space, or 0 if the
 * host's own model is the one in the hand.
 *
 * Perfect Dark fires everything from that node - the bullet stream, a beam,
 * the smoke, a projectile - and it belongs to the host. A release gun of
 * another shape drawn on it ends somewhere else.
 *
 * `outpart` is the model part the offset is measured from, which is the
 * muzzle node where the host has one and its muzzle flash where it does not -
 * Perfect Dark's conversions of GoldenEye's submachine guns and rifles carry
 * no muzzle node at all.
 */
s32 gebeanFirstPersonMuzzleOffset(s32 weaponnum, s32 *outpart, f32 *out);

/**
 * How far to move a GoldenEye gun a character holds, in the gun's own space,
 * so that the release's pickup sits in the hand where GoldenEye holds its own
 * rather than where its host's held position puts it; 0 when there is nothing
 * to move (the N64 look, a gun whose host is held from GoldenEye's own point).
 */
s32 gebeanHeldGunOffset(struct model *model, s32 modelnum, f32 out[3]);
s32 gebeanHeldGunMuzzle(struct model *model, s32 modelnum, f32 out[3]);

/**
 * How far along x (a weapon's posx units) the first-person gun is drawn from
 * where its model is put - GoldenEye's position less its host's, for a gun the
 * release's mesh draws where GoldenEye holds it - and whether there is such a
 * shift.
 */
s32 gebeanFirstPersonOwnPlaceShiftX(s32 weaponnum, f32 *dx);

/**
 * Whether the gun drawn in the hand for this weapon is made with its round in
 * the tube - the release's rocket launcher is - so that the hand's own held
 * rocket (bondgun.c) is not drawn over it.
 */
s32 gebeanFirstPersonHasRound(s32 weaponnum);

/**
 * Whether the gun drawn in first person for this weapon is the release's HD
 * mesh (the HD look) with no hand on it, which is drawn in the left hand as
 * it is rather than mirrored.
 */
s32 gebeanFirstPersonIsRelease(s32 weaponnum);
u16 gebeanFirstPersonReleaseFile(s32 weaponnum);
// Whether a file is one the release's first-person gun of this weapon is drawn on
s32 gebeanFirstPersonIsReleaseFile(s32 weaponnum, s32 filenum);
// That gun's box in its host's space, once it has been drawn (laid on) once
s32 gebeanFirstPersonHostBox(s32 weaponnum, f32 lo[3], f32 hi[3]);
// A knife's own frame there (headfitBladeFrame()), likewise
s32 gebeanFirstPersonHostBlade(s32 weaponnum, f32 axes[3][3], f32 mid[3], f32 *length);
u16 gebeanFirstPersonWatchFile(s32 weaponnum);
// A gun holding GoldenEye's glove, in the hands of a player who is not a
// GoldenEye character: its file drawn bare (else `filenum`), and whether the
// player's own hands go on it
u16 gebeanFirstPersonFileForPlayer(s32 weaponnum, u16 filenum);
s32 gebeanFirstPersonTakesPlayersHands(s32 weaponnum);
s32 gebeanFirstPersonMatrixRest(s32 weaponnum, s32 mtx, f32 out[3]);
u16 gebeanFirstPersonToOwn(s32 weaponnum, f32 tomodel[4][4], f32 ownlo[3], f32 ownhi[3]);

/**
 * GoldenEye's characters and heads in the Combat Simulator's own lists, for
 * Perfect Dark rather than for GoldenEye X: each one a row of g_HeadsAndBodies
 * past the stock table, whose file is an alias of a Perfect Dark body or head
 * (romdataRegisterAliasFile()) that the Bean mesh is skinned onto. Appended
 * when the switch is on, a copy is in xbla/ and the lists are the game's own -
 * a mod's lists (GoldenEye X's have GoldenEye's characters already) keep them
 * out - and taken off again otherwise. Called at boot, after a mod swap and
 * when the switch changes.
 */
void gebeanPoolRefresh(void);

/**
 * A GoldenEye character's release mesh has just failed to build (xblamesh.c,
 * once per failure): a pool row the release was to fill takes GoldenEye's own
 * N64 model from the ROM's conversion from then on, for the session, and a
 * converted mission's character - whose N64 model is underneath already - is
 * noted in the log. Logs "gebean: <source> HD failed (<why>), using the ROM
 * model".
 */
void gebeanBuildFailed(s32 row, u16 fileid);

/**
 * modeldefLoad()'s check of a pool row's model as it loads: builds its release
 * mesh there and then rather than at the first draw, and where that fails
 * answers the ROM model's file the row has just been given (gebeanBuildFailed()),
 * for the caller to load in its place; else 0.
 */
u16 gebeanPoolLoadCheck(struct modeldef *modeldef, u16 fileid);

/**
 * The release's meshes moved (F6). GoldenEye's guns follow that switch as its
 * characters do - the release's gun with the meshes on, GoldenEye's own N64
 * one with them off - and what has to move with it rather than at the draw is
 * whether Perfect Dark's hands are drawn, since the N64 gun carries
 * GoldenEye's own.
 */
void gebeanMeshesSwitched(void);

/** The Combat Simulator name of a pool body's g_HeadsAndBodies row, or NULL. */
const char *gebeanPoolBodyName(s32 bodynum);

/**
 * GoldenEye's name for a g_HeadsAndBodies head row that is one of its faces -
 * the release's pool, or GoldenEye X's own, borrowed or loaded - or NULL.
 */
const char *gebeanHeadName(s32 headnum);

/**
 * Makes sure the copy is on disk, unpacking it if it has to - which is a level
 * load's business, not a frame's, so a model load that finds a row calls this.
 * 1 when the files are there.
 */
s32 gebeanPrepare(void);
// The first unpack at startup, with a notice on the window while it works.
void gebeanUnpackAtStartup(void);

/**
 * Which of the model's matrices a list node is drawn under: the nearest
 * position or chrinfo node above it, and 0 for a head file's lists, which hang
 * under the body's neck (matrix 0) once grafted and under the head's one
 * matrix when the head is drawn on its own.
 */
s32 gebeanListNodeMatrix(const struct modelnode *node);

/**
 * The matrix a list node's own display list loads first (its first G_MTX), or
 * -1 when it loads none. A list is drawn under that one, whatever position
 * node it hangs under: the PP9i's gun list is under the root and loads 33.
 */
s32 gebeanListLoadedMatrix(const struct modelnode *node);
// The matrix each of a list node's vertices is loaded under, -1 before any;
// filebase is the model file's start, or NULL for a loaded model
s32 gebeanListVertexMatrices(const struct modelnode *node, const u8 *filebase, s16 *vtxmtx, s32 numvertices);
s32 gebeanIsPoolRow(s32 headorbodynum);

/**
 * Whether a head or body row is the pool's filled from the ROM's conversion
 * instead (no release present): GoldenEye's own N64 model, no mesh over it.
 */
s32 gebeanIsRomPoolRow(s32 headorbodynum);

/**
 * A Customize Character pick of one of the pool's bodies with one of its
 * heads, moved onto GoldenEye's own rig when the ROM is converted: the rows
 * of the ROM's matching body (and head, where the ROM has it), which the
 * release's meshes are fitted to as a mission guard's are. The pair is left
 * alone and 0 returned for anything else - a Perfect Dark head or body, no
 * ROM - which keeps the host rig.
 */
s32 gebeanOwnRigPair(s32 *bodynum, s32 *headnum);

/** Whether a row is one of gebeanOwnRigPair()'s. */
s32 gebeanIsOwnRigRow(s32 headorbodynum);

/**
 * GoldenEye's own character number (Cgx%03dZ) behind a pool, own-rig or extra
 * row, or -1 for any other row. A named character's face answers as its body.
 */
s32 gebeanRowGeChr(s32 headorbodynum);

/**
 * Whether a body row is one of GoldenEye's own characters for GE Plus: the
 * release's pool, or the ROM's (the pool filled from the conversion and the
 * extras the release lacks). Never GoldenEye X's - GE Plus is made of the
 * ROM and the release alone.
 */
s32 gebeanIsGoldenEyeBody(s32 bodynum);

/**
 * A random GoldenEye head (a g_HeadsAndBodies row, of the body's sex) for a
 * GoldenEye body that names none, or -1 for any other body: a GoldenEye body
 * never draws a Perfect Dark face.
 */
s32 gebeanRandomHeadForBody(s32 bodynum);

// GoldenEye's own soldier bodies installed (g_HeadsAndBodies rows), up to max
s32 gebeanGuardBodies(s32 *rows, s32 max);

// The pool's body or head for one of GoldenEye's characters by its Bean source
// ("char/oliveguard"), or -1 when the pool is not Bean's.
s32 gebeanPoolNumBySource(const char *source);

#define GEBEAN_MAXMATS 256

// A reflection's sphere map, as the XBLA meshes' atlas cells are (XBLAMESH_ENV_CELL)
#define GEBEAN_ENV_CELL 256

/**
 * A GoldenEye XBLA level's HD mesh - files/new/background/<name> - for
 * gebeanstage.c, which serves it as the rooms of GoldenEye X's copy of the
 * level.
 */
struct gebeanlevel;

struct gebeanlevelvtx {
	f32 pos[3];
	f32 uv[2];
	u32 argb;
	f32 nrm[3]; // the file's own, zero where it has none
	u8 blend; // its draw is in the release's blended pass (the same on all three)
	u8 plain; // its draw has no UV and no picture of its own (gebeanLevelTriangles())
	u8 alphatest; // its draw's alpha test is on (greater than alpharef, of picture times vertex alpha)
	u8 alpharef;
};

struct gebeanlevel *gebeanLevelOpen(const char *name);
struct gebeanlevel *gebeanLevelOpenSource(const char *source);
void gebeanLevelClose(struct gebeanlevel *level);

/**
 * A level's sky, files/new/skydome/<name>, opened as a level is: its
 * triangles and pictures through gebeanLevelTriangles()/gebeanLevelTexture(),
 * closed with gebeanLevelClose(). NULL when it is not there.
 */
struct gebeanlevel *gebeanSkyOpen(const char *name);

/**
 * One of the release's models opened for its pictures alone, which is what the
 * folder screens want of it (gefolder.c): GoldenEye's own geometry, painted
 * with the release's art. source is the path under files/, so
 * "new/prop/walletbond".
 */
struct gebeanpictures;

struct gebeanpictures *gebeanPicturesOpen(const char *source);
void gebeanPicturesClose(struct gebeanpictures *pics);
s32 gebeanPicturesCount(struct gebeanpictures *pics);
u8 *gebeanPicturesDecode(struct gebeanpictures *pics, s32 index, s32 *outWidth, s32 *outHeight);
const char *gebeanPicturesName(struct gebeanpictures *pics, s32 index);

/**
 * The model's draws, each handed to fn with its triangles expanded (three
 * vertices a triangle, positions in the file's own units, UVs as the file
 * means them, v down) - see gebeanPicturesWalk() in gebean.c. Returns how
 * many there were.
 */
#define GEBEAN_MAXCONDS 4

struct gebeanmodelvtx {
	f32 pos[3];
	f32 uv[2];
	u32 argb;
	f32 nrm[3]; // the file's own, zero where it has none
};

struct gebeanmodeldraw {
	s32 node;                    // the node a 0x30 draw names, or -1
	s32 numconds;
	s32 conds[GEBEAN_MAXCONDS];  // the 0x17 sections it stands in, outermost first
	s32 tex;                     // the model's picture index (gebeanPicturesDecode())
	f32 uvscale;                 // the file's UV units in one picture (uv = raw s16 / uvscale)
	s32 numvtx;
	struct gebeanmodelvtx *vtx;
};

s32 gebeanPicturesWalk(struct gebeanpictures *pics, void (*fn)(const struct gebeanmodeldraw *d, void *arg), void *arg);

/**
 * A picture that is a file of its own under files/ - "texture/level/damicon" -
 * as RGBA in the game's row order, malloc'd and the caller's; NULL where the
 * release or the file is not there.
 */
u8 *gebeanDecodePictureFile(const char *source, s32 *outWidth, s32 *outHeight);

/**
 * One of the menus' two fonts - "alps3", the bold sans the text is set in, or
 * "doc0", the Bank Gothic of the headings - with its picture, RGBA in the
 * game's row order (bottom-up). A glyph's box is in that picture's pixels
 * counted from the top, as the file has it; its metrics in the same pixels.
 * NULL where the release or the file is not there.
 */
struct gebeanglyph {
	u16 ch;          // Unicode
	s8 left;         // from the pen to the box's left
	u8 width;
	u8 height;
	s16 top;         // from the baseline up to the box's top
	u16 advance;     // 0 for the space, which is the font's own
	f32 u0, v0, u1, v1;
};

struct gebeanfont {
	s32 lineheight;
	s32 ascent;
	s32 space;
	s32 width, height;  // the picture's
	u8 *rgba;
	s32 numglyphs;
	struct gebeanglyph glyphs[];
};

struct gebeanfont *gebeanFontOpen(const char *name);
void gebeanFontClose(struct gebeanfont *font);

/** Where the release is: its files/, the archive it came from ("" if none), the cache. */
// The Community Edition's overlay, a folder of the release's cache
// (gebeance.c). It holds a files/new/char of its own - the characters the
// patch changes - so a scan of the cache must never take it for the release.
#define GEBEAN_CE_DIR "ce"

// The entry that makes an archive the Community Edition's updater zip, as
// opposed to either release: gebeance.c looks for it to find the updater, and
// xblaimport.c to pass it over when looking for Perfect Dark's release.
#define GEBEANCE_DIFF_ENTRY "CEUpdate/files.diff"

s32 gebeanTreeInfo(char *root, u32 rootLen, char *archive, u32 archiveLen, char *cache, u32 cacheLen);

/**
 * The GoldenEye XBLA Community Edition, applied by the game from the player's
 * own updater zip in added-content/ (gebeance.c). Chosen in the menu, drawn
 * from the next start: gebeanCeRestartNeeded() says a change is waiting.
 */
s32 gebeanCeAvailable(void);
s32 gebeanCeGetWanted(void);
void gebeanCeSetWanted(s32 on);
s32 gebeanCeIsActive(void);
s32 gebeanCeRestartNeeded(void);
void gebeanCePrepareAtStartup(void);
s32 gebeanCeFilePath(char *dst, u32 dstLen, const char *source, const char *name);
/** The overlay's copy of the environment table's rows, from the CE's patched default.xex. */
s32 gebeanCeFogTablePath(char *dst, u32 dstLen);
const char *gebeanCeLevelName(const char *key, const char *name);
s32 gebeanLevelTriangles(struct gebeanlevel *level,
		void (*fn)(void *arg, s32 tex, const struct gebeanlevelvtx *v), void *arg);
s32 gebeanLevelNumTextures(struct gebeanlevel *level);
u8 *gebeanLevelDecode(struct gebeanlevel *level, s32 tex, s32 *outWidth, s32 *outHeight);
const char *gebeanLevelTextureName(struct gebeanlevel *level, s32 tex);
/** Whether a level's picture is drawn by its water buffers (stride 36), once gebeanLevelTriangles() has walked it. */
s32 gebeanLevelTextureIsWater(struct gebeanlevel *level, s32 tex);
const void *gebeanLevelTexture(struct gebeanlevel *level, s32 tex, u8 *alpha, u8 *soft);
/** The level file's bytes as read from disk, for keying the HD level cache (gebeanstage.c). */
const u8 *gebeanLevelFileBytes(const struct gebeanlevel *level, u32 *len);
/** The game's cache folder for the release, cache/xbla/goldeneye (made when missing). 0 when there is none. */
s32 gebeanGetCacheDir(char *dst, u32 dstLen);

// The directions round the neck a pool head is seated by (gebeanmats.seat)
#define GEBEAN_SEAT_SAMPLES 16
// A head whose open rim (gebeanmats.openrim) is higher than this ends at its
// jaw: every N64-look head (-6 to -20); the release's with necks -33 to -74
#define GEBEAN_OPENRIM_JAW -30.0f

/** The pictures a built mesh's material words index (XBLAMESH_MAT_TABLE). */
struct gebeanmats {
	u16 fileid;  // in: the model file the mesh is built for
	s32 num;
	const void *tile[GEBEAN_MAXMATS];
	u8 alpha[GEBEAN_MAXMATS];
	u8 soft[GEBEAN_MAXMATS];
	// A tinted pane, which goes to GoldenEye's far-pane grey with its
	// opacity (gebeanBuildRigid(), xblamesh.c's xblaMeshTintCopy())
	u8 tinted[GEBEAN_MAXMATS];
	// Groups blanked because the head file carries this body's neck
	u64 neckblank;
	// For a neck node, the group holding the body's own neck to draw instead
	// under a head that is not its own; -1 for none
	s8 neckfill[64];
	// A hood cut off with a face but painted on the body's picture (the
	// parka's): on a body, for a neck node, the group holding the hood, drawn
	// only under the head cut off the same neck; on that head, for a list
	// node, the group of the face with the hood round it, drawn on any other
	// body (or none). -1 for none
	s8 hood[64];
	// On that body, for a neck node, the group of its own triangles the neck
	// moves as they were before the hood took copies of them, drawn when the
	// hood is not; -1 for none
	s8 bare[64];
	// On a first-person launcher made loaded (gebean.c's fpRound), for a list
	// node the round is drawn in, the group of the same gun without it, drawn
	// while the tube is empty; -1 for none
	s8 spent[64];
	// On a head, for a list node, the group of the node's own triangles with
	// a neck made under its rim (gebean.c's beanAddNeckTube()), drawn on any
	// body but the one it was made on; -1 for none
	s8 neck[64];
	u8 head;  // the mesh is a head's
	// A material the release adds a reflection over (gebean.c's
	// beanReflectPicture()): its sphere map, GEBEAN_ENV_CELL square RGBA,
	// malloc'd and the caller's to free, and how much of it, out of 255. NULL
	// and 0 for the rest. envkey names the mesh's maps for the atlas.
	u8 *env[GEBEAN_MAXMATS];
	u8 envamount[GEBEAN_MAXMATS];
	char envkey[48];
	// A head whose palette entry 1 is the joint above the one it is drawn on
	// (the back), not a second matrix of its own: xblaMeshPose() finds it on
	// the body the head is grafted to
	u8 neckback;
	// A head's rim, or a body's collar, by direction round the neck joint in
	// the neck's frame (gebean.c's beanSeatPoint()): a head's lowest, a
	// body's highest; seathit says which directions were found.
	// The pool's heads are seated on its bodies by them (xblamesh.c's
	// xblaMeshPoolHeadSeat())
	f32 seat[GEBEAN_SEAT_SAMPLES];
	u32 seathit;
	// A body's own neck joint as its fit placed it, from the model's neck
	// joint in the model's rest space (gebean.c, after beanFitRig()): where
	// GoldenEye hangs a head on it, which the pool's heads are seated by
	f32 neckshift[3];
	u8 hasneckshift;
	// How far the fit moved the collar against the model's neck joint from
	// where it stood against Bean's own neck (GoldenEye's): the lift that
	// keeps a pool head's own gap to it
	f32 collarshift;
	u8 hascollarshift;
	// A head cut off a whole character's own neck (a Bond outfit's), not a head file
	u8 fromchar;
	// A head's open rim, the middle of its edges one triangle has, in its own
	// space about the neck joint: at the jaw, or at the foot of a neck of its own
	f32 openrim;
	// A body's neck node's group of its whole neck as Bean skins it, drawn
	// under one of the pool's heads that ends at its jaw (gebean.c's ownof)
	s8 ownneck[64];
	// On a guard's body of the remake's own rows, for a list node, the group
	// of its triangles less the skin of the body's own neck below the cut,
	// drawn under one of the release's head files (gebean.c's ownskin); -1
	// for none
	s8 noskin[64];
	// A monitor's screens (parts 0 to 3) whose programme goes on Bean's own
	// pane rather than GoldenEye's quad (gebean.c's beanScreenFit()): a bit
	// per part, and the four corners in the model's space, in the order of
	// the part's own vertices (xblaMeshScreenQuad())
	u8 screenfit;
	f32 screenquad[4][4][3];
	// and the screens whose recess's back Bean's model has on GoldenEye's
	// quad, laid on its plane (beanScreenFace()): the programme is drawn over
	// it as a decal
	u8 screenrecess;
};

/**
 * The mesh for a row, in 4J's layout: group k is list node k of nodes, and a
 * bit of *outAbsent is set for a node that keeps its own geometry. The palette
 * is the model's matrices, holding each joint's inverse rest. NULL when the
 * copy is missing or the model is not the shape the row expects. The file is
 * malloc'd and the caller's. original takes the character from Bean's
 * files/original/ (the N64 look) instead of files/new/ (HD).
 */
u8 *gebeanBuild(s32 row, s32 original, struct modeldef *modeldef, struct modelnode **nodes, s32 numnodes,
		struct gebeanmats *mats, u64 *outAbsent, u32 *outLen);

#ifdef __cplusplus
}
#endif

#endif
