#ifndef _IN_X360_H
#define _IN_X360_H

#include <PR/ultratypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Reading Xbox 360 containers, for the XBLA release's assets.
 *
 * Three layers sit between a downloaded XBLA package and a picture, and none
 * of them is anything to do with Perfect Dark:
 *
 *   * STFS is the package a title ships in - "LIVE", "CON " or "PIRS". A small
 *     filesystem whose data blocks are interleaved with hash tables.
 *   * LZX is what XMemCompress produces. The chunk framing lives outside the
 *     bitstream, so it is read here and the bitstream by an LZXD decoder.
 *   * A texture is a tiled, byte-swapped, often block-compressed surface whose
 *     format is in the GPU fetch constant rather than in the data.
 *
 * The window is 17 bits. No other size decodes these streams, so a decode
 * error means the chunk framing went wrong rather than the window being off.
 */

/* -------------------------------------------------------------------------
 * LZX
 * ------------------------------------------------------------------------- */

#define X360_LZX_WINDOW_BITS 17

/**
 * Decompresses one chunked XMemCompress stream.
 *
 * src is the whole stream including its chunk headers; dst must have room for
 * dstLen bytes. The state carried between chunks is set up once here, because
 * a chunk is not an independent stream: the window and the Huffman trees
 * persist across the boundary.
 *
 * Returns the number of bytes written, or 0 if the stream did not decode.
 */
u32 x360LzxDecompress(const u8 *src, u32 srcLen, u8 *dst, u32 dstLen);

/* -------------------------------------------------------------------------
 * STFS
 * ------------------------------------------------------------------------- */

#define X360_STFS_NAMELEN 40

struct x360stfsfile {
	char name[X360_STFS_NAMELEN + 1];
	u32 size;
	u32 startBlock;
	u32 numBlocks;
	u16 parent;
	u8 isDir;
	u8 consecutive;
};

struct x360stfs {
	void *fp;              // FILE *, kept opaque so callers need no stdio
	u32 tableShift;        // 0 when one hash table precedes each block run
	u32 fileTableBlock;
	u32 fileTableBlocks;
	u32 totalBlocks;
	struct x360stfsfile *files;
	u32 numFiles;
};

/** Opens a package and reads its file table. Returns 0 if it is not one. */
s32 x360StfsOpen(struct x360stfs *stfs, const char *path);
void x360StfsClose(struct x360stfs *stfs);

/**
 * The full path of one entry, e.g. "DataFiles/Textures.raw", into buf.
 *
 * An entry names only itself and its parent's index, so a path is walked up
 * the table. A cycle in a malformed table would not terminate, so the walk is
 * bounded.
 */
const char *x360StfsPath(struct x360stfs *stfs, u32 index, char *buf, u32 bufLen);

/** Index of an entry by path, or -1. */
s32 x360StfsFind(struct x360stfs *stfs, const char *path);

/**
 * Reads one whole file out. The caller owns the returned buffer and frees it
 * with free(); *outLen gets its length. NULL if it could not be read.
 */
u8 *x360StfsRead(struct x360stfs *stfs, u32 index, u32 *outLen);

/**
 * Reading part of a file, for one too big to want in memory.
 *
 * Textures.raw is 166MB and only ever read a texture at a time, so the block
 * list is resolved once here and each read seeks straight to the right block.
 * Doing it per read instead would walk the hash chain of a non-consecutive
 * file from the start every time.
 */
struct x360stfsstream {
	struct x360stfs *stfs;
	u32 *blocks;
	u32 numBlocks;
	u32 size;
};

s32 x360StfsStreamOpen(struct x360stfs *stfs, u32 index, struct x360stfsstream *stream);
void x360StfsStreamClose(struct x360stfsstream *stream);

/** Reads len bytes at offset. Returns 0 if that is not all inside the file. */
s32 x360StfsStreamRead(struct x360stfsstream *stream, u32 offset, u32 len, u8 *dst);

/* -------------------------------------------------------------------------
 * Texture surfaces
 * ------------------------------------------------------------------------- */

// GPUTEXTURE_FETCH_CONSTANT data formats, only those these packages use.
#define X360_FMT_8888  6
#define X360_FMT_DXT1  18
#define X360_FMT_DXT23 19
#define X360_FMT_DXT45 20

struct x360fetch {
	u32 width;
	u32 height;
	u32 pitch;    // stored row width in texels
	u8 format;
	u8 endian;    // 1 is 8-in-16, 2 is 8-in-32
	u8 tiled;
};

/** Reads the six dwords of a fetch constant, which are already big-endian. */
void x360FetchRead(struct x360fetch *fetch, const u32 *dwords);

/** Whether this port can turn that format into RGBA. */
s32 x360FetchSupported(const struct x360fetch *fetch);

/**
 * Decodes a surface's base level into width * height RGBA8 pixels.
 *
 * data is modified in place - the endian swap the console needs is done there
 * rather than in a second buffer. Mip levels follow the base in the same
 * buffer and are simply left off the end: a pack only replaces level 0.
 *
 * Returns 0 if the buffer is too small for the level, which is the one thing a
 * truncated or misread record shows up as.
 */
s32 x360DecodeTexture(u8 *data, u32 dataLen, const struct x360fetch *fetch, u8 *rgba);

#ifdef __cplusplus
}
#endif

#endif
