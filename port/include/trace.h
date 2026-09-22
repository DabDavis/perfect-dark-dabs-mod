#ifndef _IN_TRACE_H
#define _IN_TRACE_H
#include <PR/ultratypes.h>

/**
 * A key that writes a state dump for the frame you are looking at, and takes
 * a screenshot to go with it. Mod.TraceKey, F3 by default.
 *
 * The dump is traces/pd-YYYYmmdd-HHMMSS.txt beside the executable (the
 * screenshot of the same frame is screenshots/pd-<same stamp>.png): the
 * memory pools, the XBLA loaders, the renderer's texture cache, the camera,
 * the rooms on screen, and every character with what chrRender() did with it
 * this frame. It is for reports like "the guards turn invisible when I turn":
 * press the key while it is happening and send both files.
 *
 * With Mod.TraceReport on (the default) the key also offers to send them:
 * a Report a Problem dialog opens over the game with a note to type, and Send
 * puts the dump, the note and a copy of the picture scaled to at most 1280
 * wide (traces/pd-<stamp>.png) on the server's /report. port/src/tracereport.c.
 */
struct chrdata;

// What chrRender() did with a chr this frame, kept in chr->tracedrawbits.
#define TRACECHR_CALLED_OPA  0x01 // the opaque pass reached it
#define TRACECHR_CALLED_XLU  0x02 // the translucent pass reached it
#define TRACECHR_BODYNODRAW  0x04 // a kept body that lost the draw budget
#define TRACECHR_EYESPY      0x08 // the eyespy, not deployed or being flown
#define TRACECHR_XRAYFAR     0x10 // beyond the IR scanner's range
#define TRACECHR_DEFERRED    0x20 // translucent, so left for the xlu pass
#define TRACECHR_NODRAW      0x40 // alpha 0 or an xlu shade mode: nothing drawn
#define TRACECHR_DREW        0x80 // modelRender() was called for it

void traceInit(void);
// Once per frame, after inputUpdate(). Reads the key.
void traceTick(void);
// Write one at the end of the frame being drawn now.
void traceRequest(void);
// VK_ value of the key, or 0 if unbound.
s32 traceGetKey(void);
// Called from chrRender() to record what happened to a chr this frame.
void traceChrNote(struct chrdata *chr, u8 bit);

// Report a Problem (port/src/tracereport.c).
#define TRACEREPORT_MAXNOTE 300
// The name a reporter wants crediting under, kept in pd.ini once typed.
#define TRACEREPORT_MAXNAME 32
// Whether F3 offers to send what it wrote.
s32 traceReportEnabled(void);
// A dump and its picture were written; offer them at the next safe moment.
void traceReportOffer(const char *tracepath, const char *shotpath);
// From lvTick(), before menuTick(): opens the dialog.
void traceReportTick(void);

#endif
