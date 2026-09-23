#ifndef _IN_XBLAAGENT4_H
#define _IN_XBLAAGENT4_H

#include <PR/ultratypes.h>

/**
 * 4J's Agent 4, the one character the XBLA release adds: its head and body rows
 * and its place in the Combat Simulator's lists, when the release is there to
 * read its files from. Called by gebeanPoolRefresh() with the lists at their
 * stock lengths; appends to them.
 */
void xblaAgent4Refresh(void);

/** "Agent 4" for his body row while he is listed, else NULL. */
const char *xblaAgent4BodyName(s32 bodynum);

/** F6 or the meshes checkbox: his own rows with the release's meshes, else the Shock Trooper's. */
void xblaAgent4MeshesSwitched(void);

/** bodiesReset(): every row's model is gone with the stage, so are the kept ones. */
void xblaAgent4StageReset(void);

#endif
