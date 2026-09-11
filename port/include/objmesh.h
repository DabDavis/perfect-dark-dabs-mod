#ifndef _IN_OBJMESH_H
#define _IN_OBJMESH_H

#include <PR/ultratypes.h>
#include "fs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A mesh in memory, halfway between every format this port dumps and loads.
 *
 * The dumper builds one of these from a Perfect Dark model or from one of the
 * XBLA release's meshes and writes it out as OBJ; the model pack loader reads
 * an OBJ back into one and hands it to the mesh builder in xblamesh.c. What
 * it holds is the XBLA mesh's own shape - vertices, triangles, draws (a run
 * of triangles under one material) and groups (a run of draws that one part
 * of the model owns) - because that is the shape the display list builder
 * already understands, and a Perfect Dark model fits it with a group per
 * list node.
 *
 * Coordinates are the game's own units. The v of a UV pair counts from the
 * bottom of the picture the way OBJ does, which is the N64's t direction
 * (t = 0 is the first row the game uploads, and our dumps write that row at
 * the bottom of the PNG) - so an OBJ written here, opened in Blender against
 * the dumped PNGs, is the right way up, and one read back needs no flip.
 *
 * Materials are named for what they draw with, so a file can be edited in
 * any modeller and still say which of the game's pictures a face wants:
 *
 *   n64_0a9a   texture number 0x0a9a of the ROM (the texture pack's number)
 *   xbla_1156  record 0x1156 of the XBLA release's Textures.raw
 *   tex4438    the same record in decimal, as tools/texpack/xblamesh.py writes
 *
 * and a material with a map_Kd whose file exists beside the OBJ is that
 * picture instead, whatever it is called.
 */

#define OBJMESH_NAMELEN 64

// What a material draws with - see objmaterial.kind.
#define OBJMAT_NONE  0 // shaded, no picture
#define OBJMAT_N64   1 // id is a texture number
#define OBJMAT_XBLA  2 // id is a Textures.raw record
#define OBJMAT_IMAGE 3 // image is a file on disk

struct objvertex {
	f32 pos[3];
	f32 uv[2];
	f32 nrm[3];
	u8 rgba[4];
	f32 weight[3];  // skinned meshes only; sum to one
	u8 bone[4];     // and the palette entries they weight, with the fourth byte as read
};

struct objdraw {
	u32 firsttri;
	u32 numtris;
	s32 material; // into materials[], or -1
};

struct objgroup {
	u32 firstdraw;
	u32 numdraws;
	u32 matrixindex;  // the XBLA group table's third word; unused otherwise
	s32 number;       // the number in a "part3"/"node3" name, or -1
	char name[OBJMESH_NAMELEN];
};

struct objmaterial {
	char name[OBJMESH_NAMELEN];
	s32 kind;   // OBJMAT_*
	u32 id;     // for OBJMAT_N64 and OBJMAT_XBLA
	s32 alpha;  // whether the picture is drawn with its alpha (the XBLA material bit)
	char image[FS_MAXPATH + 1]; // for OBJMAT_IMAGE: the file, resolved; for writing: what map_Kd says
};

struct objmesh {
	char name[OBJMESH_NAMELEN];
	struct objvertex *vertices;
	u32 numvertices;
	u32 *indices;      // three per triangle
	u32 numtris;
	struct objdraw *draws;
	u32 numdraws;
	struct objgroup *groups;
	u32 numgroups;
	struct objmaterial *materials;
	u32 nummaterials;

	// Skinning, from an XBLA mesh: the palette as the file holds it (three
	// rows of four floats each, translation in the last column, in the
	// mesh's own units), and the header's scale word.
	u32 nummatrices;
	f32 *matrices;
	f32 headerscale;
	s32 skinned;
};

struct objmesh *objmeshAlloc(const char *name);
void objmeshFree(struct objmesh *m);

/** Grow the arrays. Each returns the index of the new element, or -1. */
s32 objmeshAddVertex(struct objmesh *m, const struct objvertex *v);
s32 objmeshAddTriangle(struct objmesh *m, u32 a, u32 b, u32 c);
s32 objmeshAddDraw(struct objmesh *m, u32 firsttri, u32 numtris, s32 material);
s32 objmeshAddGroup(struct objmesh *m, const char *name, u32 firstdraw, u32 numdraws);
s32 objmeshAddMaterial(struct objmesh *m, const char *name, s32 kind, u32 id, s32 alpha, const char *image);

/** The material of that name, or -1. */
s32 objmeshFindMaterial(const struct objmesh *m, const char *name);

/**
 * Writes the OBJ and an MTL beside it (same name, .mtl). Every material's
 * image is written as its map_Kd, verbatim. comment goes at the top of the
 * file after the format line, one line per '\n'.
 */
s32 objmeshWrite(const struct objmesh *m, const char *objpath, const char *comment);

/**
 * Reads an OBJ and the MTL it names. Faces of any size are fanned into
 * triangles; a vertex is one (position, uv, normal) triple. Groups come from
 * 'g' and 'o' lines, draws from runs of faces under one 'usemtl'. Materials
 * are classified by name and by whether their map_Kd exists - see the top of
 * this file. Returns NULL and logs why on anything it cannot read.
 */
struct objmesh *objmeshRead(const char *objpath);

#ifdef __cplusplus
}
#endif

#endif
