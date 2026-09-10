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

#endif
