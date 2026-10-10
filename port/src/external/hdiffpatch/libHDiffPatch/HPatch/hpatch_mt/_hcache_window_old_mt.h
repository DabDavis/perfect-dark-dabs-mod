//  _hcache_window_old_mt.h
//  hpatch
/*
 The MIT License (MIT)
 Copyright (c) 2026 HouSisong
*/
#ifndef _hcache_window_old_mt_h
#define _hcache_window_old_mt_h
#include "_hpatch_mt.h"
#ifdef __cplusplus
extern "C" {
#endif
#if (_IS_USED_MULTITHREAD)

struct hcache_window_old_mt_t;
struct _oldwin_info_t;

size_t hcache_window_old_mt_t_memSize(void);

struct hcache_window_old_mt_t* hcache_window_old_open(void* pmem,size_t memSize,struct hpatch_mt_t* h_mt,
                                                      unsigned char* buf,size_t bufSize,
                                                      const hpatch_TStreamInput* old_stream,hpatch_StreamPos_t windowCount);

// Main thread: batch submit window old data loading, appends to existing unconsumed batch;
void hcache_window_old_prepareBatch(struct hcache_window_old_mt_t* self,hpatch_size_t batchCount,
                                    const struct _oldwin_info_t* winInfos,hpatch_size_t posStart,hpatch_size_t posMask);

// Main thread: get the data pointer for the next prepared window
// Blocks until async load completes. Returns up to 2 segments (seg2 may be empty).
hpatch_BOOL hcache_window_old_getWindow(struct hcache_window_old_mt_t* self,
                                        hpatch_StreamPos_t windowOldPos,hpatch_StreamPos_t windowOldLength,
                                        unsigned char** out_seg1_begin, unsigned char** out_seg1_end,
                                        unsigned char** out_seg2_begin, unsigned char** out_seg2_end);

// Main thread: release the current window's read barrier
void hcache_window_old_finishWindow(struct hcache_window_old_mt_t* self);

// Stop the async thread and clean up
hpatch_BOOL hcache_window_old_close(struct hcache_window_old_mt_t* self);

#endif //_IS_USED_MULTITHREAD
#ifdef __cplusplus
}
#endif
#endif //_hcache_window_old_mt_h
