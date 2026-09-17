#ifndef _IN_HEADFIT_H
#define _IN_HEADFIT_H

#include <PR/ultratypes.h>

struct modeldef;

struct headfitbody {
	f32 spot[3];     // the headspot, in the body's own space
	f32 neckbottom;  // the neck joint's lists, above the headspot
	f32 necktop;
	// The neck's top by direction round the headspot: sector k covers the
	// angle atan2(x, z) from -pi + k * 2pi/16, above the headspot; -1e9 empty
	f32 sectortop[16];
	// And how far out from the headspot the neck is there, near that top
	f32 sectorradius[16];
	s32 numverts;
};

// The top of a body's N64 neck in the direction of a point (model space, about the headspot)
f32 headfitNeckTopToward(const struct headfitbody *body, f32 dx, f32 dz);
f32 headfitNeckRadiusToward(const struct headfitbody *body, f32 dx, f32 dz);

struct headfithead {
	f32 bottom;      // the head's lowest vertex, in its own space
	f32 base;        // 2nd percentile
	f32 top;
	s32 numverts;
};

s32 headfitMeasureHead(struct modeldef *head, struct headfithead *out);
// A body's neck, measured from its file (a loaded model's lists are rewritten)
s32 headfitMeasureBodyFile(s32 filenum, struct headfitbody *out);
void headfitSurvey(void);
void headfitSurveyHeads(void);

// Whether a head on a body is fitted by measurement rather than the ROM's type table
s32 headfitWanted(s32 headnum, s32 bodynum);
// How far to move the head's vertices up (bodymodeldef unused: bodies are measured from their files)
s32 headfitOffset(struct modeldef *headmodeldef, s32 headnum, s32 bodynum, struct modeldef *bodymodeldef);
// The offset a head copy's vertices were moved by, for the release's meshes;
// measured 1 when it was fitted (not the body's own head), 0 not, -1 unchanged
void headfitNoteApplied(const struct modeldef *modeldef, s32 offset, s32 measured);
s32 headfitWasMeasured(const struct modeldef *modeldef);
s32 headfitAppliedOffset(const struct modeldef *modeldef);
void headfitReset(void);

#endif
