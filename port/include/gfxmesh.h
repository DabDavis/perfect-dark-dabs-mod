#ifndef _IN_GFXMESH_H
#define _IN_GFXMESH_H

#include <PR/ultratypes.h>
#include <PR/gbi.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A mesh the renderer keeps on the GPU and poses there (G_MESH_EXT).
 *
 * The XBLA release's meshes and GoldenEye XBLA's (xblamesh.c) are drawn as
 * Perfect Dark display lists whose vertices are named through segment 4 and
 * whose colours are named through segment 5. A skinned one used to be posed
 * a frame at a time on the CPU into a copy of its vertices - every vertex
 * through up to three bone matrices - and then every vertex of that copy was
 * transformed again by the renderer: in a match of 80 simulants in the XBLA
 * look that was 55% and 17% of the game thread. Under G_MESH_EXT the lists
 * name the bind pose, and the renderer draws them from a copy it made once,
 * posed and transformed in the vertex shader.
 *
 * What the lists must hold to: every G_VTX names vertices of `vertices`
 * (through segment 4, at any offset) with destination 0; a vertex's colour
 * byte is four times its place in the load, and the G_COL before the load
 * names the colour array at the load's first vertex - so a vertex's colour is
 * the entry of its own index in whatever array segment 5 names. That is how
 * xblaMeshWriteBatches() builds them. A list that breaks it is drawn the way
 * any list is, with the pose worked out on the CPU vertex by vertex.
 */
struct gfxmesh {
	u32 id;                // the renderer's, 0 until it first draws the mesh
	const Vtx *vertices;   // the bind pose, as the lists name it
	s32 numvertices;
	const f32 *bindpos;    // three per vertex, unrounded; NULL for a rigid mesh
	const f32 *weights;    // three per vertex
	const u8 *bones;       // four per vertex: three palette entries and how many count
	s32 nummatrices;
	const f32 *normals;    // three per vertex, unit length, or NULL: where a skinned mesh is lit
	                       // or reflects, its normal is this posed, not its colour's three bytes
	u8 room;               // a room's vertices (roommesh.c): see below
	u8 dynamic;            // a room whose s and t dyntex rewrites from frame to frame
};

/*
 * A room (gfxmesh.room) is drawn from the same copy on the GPU, but its lists
 * are the level's and hold to none of the layout above. Its loads name its
 * vertices in any order, at any slot, and a triangle may name a vertex an
 * earlier run of the list loaded; a vertex's colour byte indexes what the
 * G_COL before its load named, somewhere in the room's colour table (the
 * draw's `colours`, made afresh every frame by roomHighlight()). The renderer
 * reads each run once, learns each vertex's place in the table and keeps the
 * run's triangles, and every frame draws them with the table as it is then:
 * a table of up to GFXMESH_ROOM_PALETTE entries goes to the vertex shader
 * whole (a vertex carries its place in it), a bigger one is gathered into a
 * colour per vertex. Anything it cannot do is drawn the way any list is.
 */
#define GFXMESH_ROOM_PALETTE 192

/**
 * A palette entry: the matrix that takes a bind-pose position to the space
 * the lists are drawn in, as three rows of four - the x, y and z of the result
 * are each a dot product of (x, y, z, 1) with one row. A row-vector Mtxf m
 * gives row j = (m[0][j], m[1][j], m[2][j], m[3][j]).
 */
#define GFXMESH_PALETTE_FLOATS 12

/** The most entries a palette may have (GFX_MESH_PALETTE_MAX, the renderer's). */
#define GFXMESH_PALETTE_MAX 64

/** One frame's draw of a mesh: the payload of a gSPMeshEXT(). */
struct gfxmeshdraw {
	struct gfxmesh *mesh;
	const f32 *palette;    // mesh->nummatrices entries, or NULL for the bind pose as it stands
	const void *colours;   // a room's: its colour table this frame, four bytes an entry
	s32 numcolours;
};

/**
 * Whether a G_MESH_EXT drawn this frame will be posed on the GPU (the
 * Video.GpuVertices setting, and a backend that can). When it is not, a mesh
 * should be posed and drawn the way it always was - the renderer would pose
 * every vertex on the CPU itself, which is no faster.
 */
s32 gfxMeshGpuAvailable(void);

/**
 * The mesh's arrays are about to be freed or replaced: the GPU's copy goes
 * with them, and the mesh is registered afresh if it is drawn again.
 */
void gfxMeshForget(struct gfxmesh *mesh);

#ifdef __cplusplus
}
#endif

#endif
