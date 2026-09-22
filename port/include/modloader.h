#ifndef _IN_PORT_MODLOADER_H
#define _IN_PORT_MODLOADER_H

#include <PR/ultratypes.h>

// GoldenEye's twenty solo missions, as its own mission folder orders them
#define MODLOADER_MAX_MISSIONS 20

void modloaderInit(void);
void modloaderGetStats(s32 *registered, s32 *found, s32 *mods);
const char *modloaderGetStageModDir(s32 stagenum);
// The same directory as an index into the mounted list, or -1. What a texture
// loaded for that stage is registered under, so its own pack can be found
// again at the draw - see texpackTextureArt().
s32 modloaderGetStageModDirIndex(s32 stagenum);
const char *modloaderGetStageAllocation(s32 stagenum);
// A Stage Loader map's own name, as its mod calls it, or NULL for a stock stage.
const char *modloaderGetStageMapName(s32 stagenum);
struct fogenvironment;

// Before a stage's setup loads: the remake's model states, from the stage's
// mod's `models` block (empty for any other stage).
void modloaderApplyStageModels(s32 stagenum);

// GE Plus, the GoldenEye remake's Combat Simulator: on while its arena list
// is the remake's arenas only.
extern s32 g_GexPlusMode;
s32 modloaderStageIsRemake(s32 stagenum);

// The GoldenEye remake's solo missions, from a mod's `missions` block: the
// stage mission n registered as (0 for none), and how many there are.
s32 modloaderMissionStage(s32 mission);
s32 modloaderNumMissions(void);
s32 modloaderStageIsMission(s32 stagenum);
// and the other way round: the mission a stage is, or -1
s32 modloaderStageMission(s32 stagenum);

// A Stage Loader map's setup with a borrowed mod's objects, and the solo setup
// of the mod's stage they come from; 0 for none.
s32 modloaderGetStageProps(s32 stagenum, const char **from);

// A Stage Loader map's fog table row from its mod's maps block, or NULL.
struct fogenvironment *modloaderGetStageFog(s32 stagenum);
struct nofogenvironment *modloaderGetStageNoFog(s32 stagenum);

// A Stage Loader map's music from its mod's maps or missions block - main
// theme, background, X theme as the mod's own sequence numbers, -1 for none -
// and false when its line gives none.
s32 modloaderGetStageMusic(s32 stagenum, s32 *tracks);

#endif
