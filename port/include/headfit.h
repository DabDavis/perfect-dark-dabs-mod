#ifndef _IN_HEADFIT_H
#define _IN_HEADFIT_H

#include <PR/ultratypes.h>

struct modeldef;

struct headfitbody {
	f32 spot[3];     // the headspot, in the body's own space
	f32 neckbottom;  // the neck joint's lists, above the headspot
	f32 necktop;
	s32 numverts;
};

struct headfithead {
	f32 bottom;      // the head's lowest vertex, in its own space
	f32 base;        // 2nd percentile
	f32 top;
	s32 numverts;
};

s32 headfitMeasureHead(struct modeldef *head, struct headfithead *out);
s32 headfitMeasureBody(struct modeldef *body, struct headfitbody *out);
void headfitSurvey(void);

// Whether a head on a body is fitted by measurement rather than the ROM's type table
s32 headfitWanted(s32 headnum, s32 bodynum);
// How far to move the head's vertices up; bodymodeldef is the loaded body, or NULL
s32 headfitOffset(struct modeldef *headmodeldef, s32 headnum, s32 bodynum, struct modeldef *bodymodeldef);
// The offset a head copy's vertices were moved by, for the release's meshes;
// measured 1 when it was fitted (not the body's own head), 0 not, -1 unchanged
void headfitNoteApplied(const struct modeldef *modeldef, s32 offset, s32 measured);
s32 headfitWasMeasured(const struct modeldef *modeldef);
s32 headfitAppliedOffset(const struct modeldef *modeldef);
void headfitReset(void);

#endif
