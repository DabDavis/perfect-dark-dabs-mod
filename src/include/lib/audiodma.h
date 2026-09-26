#ifndef _IN_LIB_AUDIODMA_H
#define _IN_LIB_AUDIODMA_H
#include <ultra64.h>
#include "data.h"
#include "types.h"

// Every sample fetch copies a whole item from its start, whatever the sound
// needs of it, so a sample table held in a heap block needs this much zeroed
// room after its end
#define ADMA_ITEM_SIZE 0x400

struct admastate;

void admaInit(void);
void *admaNew(struct admastate **state);
void admaBeginFrame(void);
void admaReceiveAll(void);

#endif
