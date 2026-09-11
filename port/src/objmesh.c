/**
 * The mesh in the middle: OBJ in, OBJ out. See objmesh.h.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <PR/ultratypes.h>
#include "system.h"
#include "fs.h"
#include "objmesh.h"

#ifndef PLATFORM_N64

#define OBJ_LINE 1024

static void *objGrow(void *array, u32 *cap, u32 need, size_t elem)
{
	void *grown;
	u32 newcap = *cap ? *cap : 64;

	if (need <= *cap) {
		return array;
	}

	while (newcap < need) {
		newcap *= 2;
	}

	grown = realloc(array, (size_t)newcap * elem);

	if (!grown) {
		return NULL;
	}

	*cap = newcap;

	return grown;
}

// The capacities live beside the mesh rather than in it, so the struct the
// rest of the port sees stays a plain description of a mesh.
struct objcaps {
	u32 vertices, indices, draws, groups, materials;
};

struct objmeshpriv {
	struct objmesh m;
	struct objcaps cap;
};

struct objmesh *objmeshAlloc(const char *name)
{
	struct objmeshpriv *p = calloc(1, sizeof(*p));

	if (!p) {
		return NULL;
	}

	if (name) {
		strncpy(p->m.name, name, sizeof(p->m.name) - 1);
	}

	return &p->m;
}

void objmeshFree(struct objmesh *m)
{
	if (!m) {
		return;
	}

	free(m->vertices);
	free(m->indices);
	free(m->draws);
	free(m->groups);
	free(m->materials);
	free(m->matrices);
	free(m);
}

static struct objcaps *objCaps(struct objmesh *m)
{
	return &((struct objmeshpriv *)m)->cap;
}

s32 objmeshAddVertex(struct objmesh *m, const struct objvertex *v)
{
	struct objvertex *grown = objGrow(m->vertices, &objCaps(m)->vertices,
			m->numvertices + 1, sizeof(*v));

	if (!grown) {
		return -1;
	}

	m->vertices = grown;
	m->vertices[m->numvertices] = *v;

	return (s32)m->numvertices++;
}

s32 objmeshAddTriangle(struct objmesh *m, u32 a, u32 b, u32 c)
{
	u32 *grown = objGrow(m->indices, &objCaps(m)->indices,
			(m->numtris + 1) * 3, sizeof(u32));

	if (!grown) {
		return -1;
	}

	m->indices = grown;
	m->indices[m->numtris * 3] = a;
	m->indices[m->numtris * 3 + 1] = b;
	m->indices[m->numtris * 3 + 2] = c;

	return (s32)m->numtris++;
}

s32 objmeshAddDraw(struct objmesh *m, u32 firsttri, u32 numtris, s32 material)
{
	struct objdraw *grown = objGrow(m->draws, &objCaps(m)->draws,
			m->numdraws + 1, sizeof(*grown));

	if (!grown) {
		return -1;
	}

	m->draws = grown;
	m->draws[m->numdraws].firsttri = firsttri;
	m->draws[m->numdraws].numtris = numtris;
	m->draws[m->numdraws].material = material;

	return (s32)m->numdraws++;
}

/** The number at the end of a "part3" or "node12" name, or -1. */
static s32 objNameNumber(const char *name)
{
	const char *p = name;
	s32 n = 0;
	s32 any = 0;

	while (*p && !isdigit((unsigned char)*p)) {
		p++;
	}

	if (p == name) {
		return -1;
	}

	while (isdigit((unsigned char)*p)) {
		n = n * 10 + (*p - '0');
		any = 1;
		p++;
	}

	return (any && *p == '\0') ? n : -1;
}

s32 objmeshAddGroup(struct objmesh *m, const char *name, u32 firstdraw, u32 numdraws)
{
	struct objgroup *grown = objGrow(m->groups, &objCaps(m)->groups,
			m->numgroups + 1, sizeof(*grown));
	struct objgroup *g;

	if (!grown) {
		return -1;
	}

	m->groups = grown;
	g = &m->groups[m->numgroups];
	memset(g, 0, sizeof(*g));
	g->firstdraw = firstdraw;
	g->numdraws = numdraws;
	g->number = -1;

	if (name) {
		strncpy(g->name, name, sizeof(g->name) - 1);
		g->number = objNameNumber(g->name);
	}

	return (s32)m->numgroups++;
}

s32 objmeshFindMaterial(const struct objmesh *m, const char *name)
{
	for (u32 i = 0; i < m->nummaterials; i++) {
		if (!strcmp(m->materials[i].name, name)) {
			return (s32)i;
		}
	}

	return -1;
}

s32 objmeshAddMaterial(struct objmesh *m, const char *name, s32 kind, u32 id, s32 alpha, const char *image)
{
	struct objmaterial *grown;
	struct objmaterial *mat;
	s32 found = objmeshFindMaterial(m, name);

	if (found >= 0) {
		return found;
	}

	grown = objGrow(m->materials, &objCaps(m)->materials,
			m->nummaterials + 1, sizeof(*grown));

	if (!grown) {
		return -1;
	}

	m->materials = grown;
	mat = &m->materials[m->nummaterials];
	memset(mat, 0, sizeof(*mat));
	strncpy(mat->name, name, sizeof(mat->name) - 1);
	mat->kind = kind;
	mat->id = id;
	mat->alpha = alpha;

	if (image) {
		strncpy(mat->image, image, sizeof(mat->image) - 1);
	}

	return (s32)m->nummaterials++;
}

/* -------------------------------------------------------------------------
 * Writing
 * ------------------------------------------------------------------------- */

static void objBaseName(const char *path, char *dst, u32 dstLen)
{
	const char *slash = strrchr(path, '/');
	const char *bslash = strrchr(path, '\\');
	const char *base = path;

	if (slash && slash + 1 > base) {
		base = slash + 1;
	}

	if (bslash && bslash + 1 > base) {
		base = bslash + 1;
	}

	strncpy(dst, base, dstLen - 1);
	dst[dstLen - 1] = '\0';
}

s32 objmeshWrite(const struct objmesh *m, const char *objpath, const char *comment)
{
	char mtlpath[FS_MAXPATH + 1];
	char mtlname[FS_MAXPATH + 1];
	FILE *f;
	size_t len = strlen(objpath);

	// The .mtl goes beside the .obj under the same name.
	snprintf(mtlpath, sizeof(mtlpath), "%s", objpath);

	if (len > 4 && !strcmp(objpath + len - 4, ".obj")) {
		mtlpath[len - 4] = '\0';
	}

	strncat(mtlpath, ".mtl", sizeof(mtlpath) - strlen(mtlpath) - 1);
	objBaseName(mtlpath, mtlname, sizeof(mtlname));

	if (m->nummaterials) {
		f = fopen(mtlpath, "wb");

		if (!f) {
			sysLogPrintf(LOG_ERROR, "objmesh: could not write %s", mtlpath);
			return 0;
		}

		fprintf(f, "# Perfect Dark model materials\n");

		for (u32 i = 0; i < m->nummaterials; i++) {
			const struct objmaterial *mat = &m->materials[i];

			fprintf(f, "newmtl %s\nKd 1 1 1\n", mat->name);

			if (mat->image[0]) {
				fprintf(f, "map_Kd %s\n", mat->image);

				if (mat->alpha) {
					fprintf(f, "map_d %s\n", mat->image);
				}
			}

			fprintf(f, "\n");
		}

		fclose(f);
	}

	f = fopen(objpath, "wb");

	if (!f) {
		sysLogPrintf(LOG_ERROR, "objmesh: could not write %s", objpath);
		return 0;
	}

	fprintf(f, "# Perfect Dark %s dump: %u vertices, %u triangles, %u draws, %u groups%s\n",
			m->skinned ? "skinned mesh" : "mesh", m->numvertices, m->numtris,
			m->numdraws, m->numgroups, m->skinned ? " (skinning is not carried by OBJ; a replacement is skinned by nearest vertex)" : "");

	if (comment) {
		const char *p = comment;

		while (*p) {
			const char *nl = strchr(p, '\n');
			size_t n = nl ? (size_t)(nl - p) : strlen(p);

			fprintf(f, "# %.*s\n", (int)n, p);
			p += n;

			if (*p == '\n') {
				p++;
			}
		}
	}

	if (m->nummaterials) {
		fprintf(f, "mtllib %s\n", mtlname);
	}

	fprintf(f, "o %s\n", m->name[0] ? m->name : "mesh");

	for (u32 i = 0; i < m->numvertices; i++) {
		const struct objvertex *v = &m->vertices[i];

		// A vertex colour rides on the v line the way Blender and MeshLab
		// write one, and is left off when it is white so a plain reader sees
		// a plain file.
		if (v->rgba[0] != 0xff || v->rgba[1] != 0xff || v->rgba[2] != 0xff) {
			fprintf(f, "v %.4f %.4f %.4f %.4f %.4f %.4f\n", v->pos[0], v->pos[1], v->pos[2],
					v->rgba[0] / 255.0f, v->rgba[1] / 255.0f, v->rgba[2] / 255.0f);
		} else {
			fprintf(f, "v %.4f %.4f %.4f\n", v->pos[0], v->pos[1], v->pos[2]);
		}
	}

	for (u32 i = 0; i < m->numvertices; i++) {
		fprintf(f, "vt %.6f %.6f\n", m->vertices[i].uv[0], m->vertices[i].uv[1]);
	}

	for (u32 i = 0; i < m->numvertices; i++) {
		fprintf(f, "vn %.4f %.4f %.4f\n", m->vertices[i].nrm[0], m->vertices[i].nrm[1], m->vertices[i].nrm[2]);
	}

	for (u32 g = 0; g < m->numgroups; g++) {
		const struct objgroup *group = &m->groups[g];

		fprintf(f, "g %s\n", group->name[0] ? group->name : "group");

		for (u32 d = group->firstdraw; d < group->firstdraw + group->numdraws && d < m->numdraws; d++) {
			const struct objdraw *draw = &m->draws[d];

			if (draw->material >= 0 && (u32)draw->material < m->nummaterials) {
				fprintf(f, "usemtl %s\n", m->materials[draw->material].name);
			}

			for (u32 t = draw->firsttri; t < draw->firsttri + draw->numtris && t < m->numtris; t++) {
				const u32 a = m->indices[t * 3] + 1;
				const u32 b = m->indices[t * 3 + 1] + 1;
				const u32 c = m->indices[t * 3 + 2] + 1;

				fprintf(f, "f %u/%u/%u %u/%u/%u %u/%u/%u\n", a, a, a, b, b, b, c, c, c);
			}
		}
	}

	fclose(f);

	return 1;
}

/* -------------------------------------------------------------------------
 * Reading
 * ------------------------------------------------------------------------- */

struct objfloat3 {
	f32 v[3];
};

// The OBJ's own arrays, which face lines index into.
struct objsource {
	struct objfloat3 *pos;  u32 numpos,  cappos;
	struct objfloat3 *col;  u32 numcol,  capcol; // beside pos; a flag says whether a line had one
	u8 *hascol;             u32 caphascol;
	struct objfloat3 *uv;   u32 numuv,   capuv;
	struct objfloat3 *nrm;  u32 numnrm,  capnrm;

	// (position, uv, normal) triples already emitted, open addressed.
	struct objkey { s32 p, t, n; s32 vertex; } *keys;
	u32 keysize;
	u32 numkeys;
};

static u32 objKeyHash(s32 p, s32 t, s32 n)
{
	u32 h = (u32)p * 2654435761u;
	h ^= (u32)t * 2246822519u;
	h ^= (u32)n * 3266489917u;
	h ^= h >> 15;
	return h;
}

static s32 objEmitVertex(struct objmesh *m, struct objsource *src, s32 p, s32 t, s32 n)
{
	struct objvertex v;
	u32 slot;
	s32 index;

	if (p < 0 || (u32)p >= src->numpos) {
		return -1;
	}

	if (src->numkeys * 2 >= src->keysize) {
		u32 newsize = src->keysize ? src->keysize * 2 : 1024;
		struct objkey *grown = calloc(newsize, sizeof(*grown));

		if (!grown) {
			return -1;
		}

		for (u32 i = 0; i < newsize; i++) {
			grown[i].vertex = -1;
		}

		for (u32 i = 0; i < src->keysize; i++) {
			if (src->keys[i].vertex >= 0) {
				u32 s = objKeyHash(src->keys[i].p, src->keys[i].t, src->keys[i].n) & (newsize - 1);

				while (grown[s].vertex >= 0) {
					s = (s + 1) & (newsize - 1);
				}

				grown[s] = src->keys[i];
			}
		}

		free(src->keys);
		src->keys = grown;
		src->keysize = newsize;
	}

	slot = objKeyHash(p, t, n) & (src->keysize - 1);

	while (src->keys[slot].vertex >= 0) {
		if (src->keys[slot].p == p && src->keys[slot].t == t && src->keys[slot].n == n) {
			return src->keys[slot].vertex;
		}

		slot = (slot + 1) & (src->keysize - 1);
	}

	memset(&v, 0, sizeof(v));
	v.pos[0] = src->pos[p].v[0];
	v.pos[1] = src->pos[p].v[1];
	v.pos[2] = src->pos[p].v[2];

	if (t >= 0 && (u32)t < src->numuv) {
		v.uv[0] = src->uv[t].v[0];
		v.uv[1] = src->uv[t].v[1];
	}

	if (n >= 0 && (u32)n < src->numnrm) {
		v.nrm[0] = src->nrm[n].v[0];
		v.nrm[1] = src->nrm[n].v[1];
		v.nrm[2] = src->nrm[n].v[2];
	} else {
		v.nrm[1] = 1.0f;
	}

	v.rgba[0] = v.rgba[1] = v.rgba[2] = v.rgba[3] = 0xff;

	if (src->hascol && src->hascol[p]) {
		for (s32 i = 0; i < 3; i++) {
			f32 c = src->col[p].v[i];

			if (c < 0.0f) {
				c = 0.0f;
			}

			if (c > 1.0f) {
				c = 1.0f;
			}

			v.rgba[i] = (u8)(c * 255.0f + 0.5f);
		}
	}

	v.weight[0] = 1.0f;

	index = objmeshAddVertex(m, &v);

	if (index < 0) {
		return -1;
	}

	src->keys[slot].p = p;
	src->keys[slot].t = t;
	src->keys[slot].n = n;
	src->keys[slot].vertex = index;
	src->numkeys++;

	return index;
}

/** One "a/b/c" of a face line: OBJ counts from one and a negative counts back. */
static s32 objParseIndex(const char **pp, u32 count)
{
	const char *p = *pp;
	s32 sign = 1;
	s32 n = 0;
	s32 any = 0;

	if (*p == '-') {
		sign = -1;
		p++;
	}

	while (isdigit((unsigned char)*p)) {
		n = n * 10 + (*p - '0');
		any = 1;
		p++;
	}

	*pp = p;

	if (!any) {
		return -1;
	}

	if (sign < 0) {
		return (s32)count - n;
	}

	return n - 1;
}

static void objDirName(const char *path, char *dst, u32 dstLen)
{
	const char *slash = strrchr(path, '/');
	const char *bslash = strrchr(path, '\\');
	const char *end = NULL;

	if (slash) {
		end = slash;
	}

	if (bslash && (!end || bslash > end)) {
		end = bslash;
	}

	if (!end) {
		snprintf(dst, dstLen, ".");
		return;
	}

	snprintf(dst, dstLen, "%.*s", (int)(end - path), path);
}

/** A hex number after a prefix, or -1. */
static s32 objHexAfter(const char *name, const char *prefix)
{
	size_t plen = strlen(prefix);
	const char *p;
	s32 n = 0;
	s32 any = 0;

	if (strncmp(name, prefix, plen)) {
		return -1;
	}

	p = name + plen;

	while (isxdigit((unsigned char)*p)) {
		const char c = (char)tolower((unsigned char)*p);
		n = n * 16 + (c >= 'a' ? c - 'a' + 10 : c - '0');
		any = 1;
		p++;
	}

	return (any && *p == '\0') ? n : -1;
}

/** A decimal number after a prefix, or -1. */
static s32 objDecAfter(const char *name, const char *prefix)
{
	size_t plen = strlen(prefix);
	const char *p;
	s32 n = 0;
	s32 any = 0;

	if (strncmp(name, prefix, plen)) {
		return -1;
	}

	p = name + plen;

	while (isdigit((unsigned char)*p)) {
		n = n * 10 + (*p - '0');
		any = 1;
		p++;
	}

	return (any && *p == '\0') ? n : -1;
}

/**
 * What a material is, from its name and its picture.
 *
 * A picture that exists beside the file wins, since that is the one thing the
 * person who wrote the OBJ definitely meant. Otherwise the name says which of
 * the game's pictures to draw with, and a name that says nothing draws shaded.
 */
/**
 * Whether a map_Kd is a picture the model comes with, rather than a pointer
 * at one of the game's.
 *
 * The dump writes both for a material that draws with one of the ROM's
 * textures: the name says which number it is, and `map_Kd` points out of the
 * pack at the picture under texture-dumps/ so that the OBJ opens with its art
 * in a modeller. Only the first of those is what the material *is* - a picture
 * reached by climbing out of the folder is a copy of something the game has -
 * and taking the second literally is what stopped a texture pack from ever
 * repainting a model pack's mesh: every material came out a picture of its
 * own, bound once and never asked about again.
 *
 * So a path that stays inside the model's own folder is the author's picture
 * and wins; one that leaves it is a reference, and the number in the name is
 * what the material draws with.
 */
static s32 objImageIsOwn(const char *image)
{
	if (fsPathIsAbsolute(image)) {
		return 0;
	}

	if (!strncmp(image, "../", 3) || !strncmp(image, "..\\", 3)) {
		return 0;
	}

	return 1;
}

static void objClassifyMaterial(struct objmaterial *mat, const char *objdir)
{
	char path[FS_MAXPATH + 1];
	s32 haveimage = 0;
	s32 own = 0;
	s32 n;

	if (mat->image[0]) {
		own = objImageIsOwn(mat->image);

		if (fsPathIsAbsolute(mat->image)) {
			snprintf(path, sizeof(path), "%s", mat->image);
		} else {
			snprintf(path, sizeof(path), "%s/%s", objdir, mat->image);
		}

		haveimage = fsFileSize(path) > 0;
	}

	if (haveimage && own) {
		strncpy(mat->image, path, sizeof(mat->image) - 1);
		mat->image[sizeof(mat->image) - 1] = '\0';
		mat->kind = OBJMAT_IMAGE;
		return;
	}

	if ((n = objHexAfter(mat->name, "n64_")) >= 0) {
		mat->kind = OBJMAT_N64;
		mat->id = (u32)n;
	} else if ((n = objHexAfter(mat->name, "xbla_")) >= 0) {
		mat->kind = OBJMAT_XBLA;
		mat->id = (u32)n;
	} else if ((n = objDecAfter(mat->name, "tex")) >= 0) {
		mat->kind = OBJMAT_XBLA;
		mat->id = (u32)n;
	} else if (haveimage) {
		// Nothing in the name to go on, so the picture it points at is all
		// there is, wherever it lives.
		strncpy(mat->image, path, sizeof(mat->image) - 1);
		mat->image[sizeof(mat->image) - 1] = '\0';
		mat->kind = OBJMAT_IMAGE;
		return;
	} else {
		mat->kind = OBJMAT_NONE;
	}

	mat->image[0] = '\0';
}

static void objReadMtl(struct objmesh *m, const char *mtlpath, const char *objdir)
{
	FILE *f = fopen(mtlpath, "rb");
	char line[OBJ_LINE];
	s32 current = -1;

	if (!f) {
		sysLogPrintf(LOG_WARNING, "objmesh: no %s; materials go by name", mtlpath);
		return;
	}

	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		char *end;

		while (isspace((unsigned char)*p)) {
			p++;
		}

		end = p + strlen(p);

		while (end > p && isspace((unsigned char)end[-1])) {
			*--end = '\0';
		}

		if (!strncmp(p, "newmtl ", 7)) {
			current = objmeshAddMaterial(m, p + 7, OBJMAT_NONE, 0, 0, NULL);
		} else if (!strncmp(p, "map_Kd ", 7) && current >= 0) {
			char *name = p + 7;

			// Options (-s, -o, ...) come before the file name.
			while (*name == '-') {
				char *sp = strchr(name, ' ');

				if (!sp) {
					break;
				}

				name = sp + 1;

				while (*name == ' ') {
					name++;
				}

				// An option takes one argument, or three for the vectors.
				if (isdigit((unsigned char)*name) || *name == '.' || *name == '-') {
					while (*name && (isdigit((unsigned char)*name) || *name == '.' ||
							*name == '-' || *name == ' ')) {
						name++;
					}
				}
			}

			strncpy(m->materials[current].image, name, sizeof(m->materials[current].image) - 1);
		} else if (!strncmp(p, "map_d ", 6) && current >= 0) {
			m->materials[current].alpha = 1;
		} else if (!strncmp(p, "d ", 2) && current >= 0) {
			if (atof(p + 2) < 0.999) {
				m->materials[current].alpha = 1;
			}
		}
	}

	fclose(f);

	for (u32 i = 0; i < m->nummaterials; i++) {
		objClassifyMaterial(&m->materials[i], objdir);
	}
}

struct objmesh *objmeshRead(const char *objpath)
{
	struct objmesh *m;
	struct objsource src;
	FILE *f;
	char line[OBJ_LINE];
	char objdir[FS_MAXPATH + 1];
	char name[OBJMESH_NAMELEN];
	s32 material = -1;
	s32 curdraw = -1;
	s32 curgroup = -1;
	s32 mtlread = 0;
	s32 ok = 1;
	s32 badfaces = 0;
	char pendinggroup[OBJMESH_NAMELEN] = "";

	f = fopen(objpath, "rb");

	if (!f) {
		sysLogPrintf(LOG_ERROR, "objmesh: could not open %s", objpath);
		return NULL;
	}

	objBaseName(objpath, name, sizeof(name));

	if (strlen(name) > 4 && !strcmp(name + strlen(name) - 4, ".obj")) {
		name[strlen(name) - 4] = '\0';
	}

	m = objmeshAlloc(name);

	if (!m) {
		fclose(f);
		return NULL;
	}

	memset(&src, 0, sizeof(src));
	objDirName(objpath, objdir, sizeof(objdir));

	while (fgets(line, sizeof(line), f)) {
		char *p = line;

		while (*p == ' ' || *p == '\t') {
			p++;
		}

		if (p[0] == 'v' && p[1] == ' ') {
			struct objfloat3 pos;
			struct objfloat3 col;
			s32 n = sscanf(p + 2, "%f %f %f %f %f %f", &pos.v[0], &pos.v[1], &pos.v[2],
					&col.v[0], &col.v[1], &col.v[2]);

			if (n < 3) {
				continue;
			}

			src.pos = objGrow(src.pos, &src.cappos, src.numpos + 1, sizeof(pos));
			src.col = objGrow(src.col, &src.capcol, src.numpos + 1, sizeof(col));
			src.hascol = objGrow(src.hascol, &src.caphascol, src.numpos + 1, 1);

			if (!src.pos || !src.col || !src.hascol) {
				ok = 0;
				break;
			}

			src.pos[src.numpos] = pos;
			src.col[src.numpos] = col;
			src.hascol[src.numpos] = n >= 6;
			src.numpos++;
		} else if (p[0] == 'v' && p[1] == 't' && p[2] == ' ') {
			struct objfloat3 uv = { { 0, 0, 0 } };

			if (sscanf(p + 3, "%f %f", &uv.v[0], &uv.v[1]) < 1) {
				continue;
			}

			src.uv = objGrow(src.uv, &src.capuv, src.numuv + 1, sizeof(uv));

			if (!src.uv) {
				ok = 0;
				break;
			}

			src.uv[src.numuv++] = uv;
		} else if (p[0] == 'v' && p[1] == 'n' && p[2] == ' ') {
			struct objfloat3 nrm = { { 0, 1, 0 } };

			if (sscanf(p + 3, "%f %f %f", &nrm.v[0], &nrm.v[1], &nrm.v[2]) < 3) {
				continue;
			}

			src.nrm = objGrow(src.nrm, &src.capnrm, src.numnrm + 1, sizeof(nrm));

			if (!src.nrm) {
				ok = 0;
				break;
			}

			src.nrm[src.numnrm++] = nrm;
		} else if (p[0] == 'f' && p[1] == ' ') {
			const char *q = p + 2;
			s32 verts[64];
			s32 numverts = 0;

			while (*q && numverts < 64) {
				s32 vp;
				s32 vt = -1;
				s32 vn = -1;
				s32 emitted;

				while (*q == ' ' || *q == '\t') {
					q++;
				}

				if (*q == '\0' || *q == '\n' || *q == '\r' || *q == '#') {
					break;
				}

				vp = objParseIndex(&q, src.numpos);

				if (*q == '/') {
					q++;

					if (*q != '/') {
						vt = objParseIndex(&q, src.numuv);
					}

					if (*q == '/') {
						q++;
						vn = objParseIndex(&q, src.numnrm);
					}
				}

				emitted = objEmitVertex(m, &src, vp, vt, vn);

				if (emitted < 0) {
					badfaces++;
					numverts = 0;
					break;
				}

				verts[numverts++] = emitted;
			}

			if (numverts < 3) {
				continue;
			}

			// A face opens a group if none is open, and a draw if the
			// material has changed since the last face.
			if (curgroup < 0) {
				curgroup = objmeshAddGroup(m, pendinggroup[0] ? pendinggroup : NULL, m->numdraws, 0);
				pendinggroup[0] = '\0';
				curdraw = -1;

				if (curgroup < 0) {
					ok = 0;
					break;
				}
			}

			if (curdraw < 0 || m->draws[curdraw].material != material) {
				curdraw = objmeshAddDraw(m, m->numtris, 0, material);

				if (curdraw < 0) {
					ok = 0;
					break;
				}

				m->groups[curgroup].numdraws++;
			}

			for (s32 i = 1; i + 1 < numverts; i++) {
				if (objmeshAddTriangle(m, (u32)verts[0], (u32)verts[i], (u32)verts[i + 1]) < 0) {
					ok = 0;
					break;
				}

				m->draws[curdraw].numtris++;
			}

			if (!ok) {
				break;
			}
		} else if ((p[0] == 'g' || p[0] == 'o') && (p[1] == ' ' || p[1] == '\n' || p[1] == '\r' || p[1] == '\0')) {
			char *q = p + 1;
			char *end;

			while (*q == ' ' || *q == '\t') {
				q++;
			}

			end = q + strlen(q);

			while (end > q && isspace((unsigned char)end[-1])) {
				*--end = '\0';
			}

			// The object name names the mesh; a group after that starts a
			// part. An 'o' with no 'g' lines is a part on its own.
			if (p[0] == 'o' && !m->name[0]) {
				strncpy(m->name, q, sizeof(m->name) - 1);
			}

			strncpy(pendinggroup, q, sizeof(pendinggroup) - 1);
			curgroup = -1;
			curdraw = -1;
		} else if (!strncmp(p, "usemtl ", 7)) {
			char *q = p + 7;
			char *end = q + strlen(q);

			while (end > q && isspace((unsigned char)end[-1])) {
				*--end = '\0';
			}

			material = objmeshAddMaterial(m, q, OBJMAT_NONE, 0, 0, NULL);
		} else if (!strncmp(p, "mtllib ", 7) && !mtlread) {
			char *q = p + 7;
			char *end = q + strlen(q);
			char mtlpath[FS_MAXPATH + 1];

			while (end > q && isspace((unsigned char)end[-1])) {
				*--end = '\0';
			}

			if (fsPathIsAbsolute(q)) {
				snprintf(mtlpath, sizeof(mtlpath), "%s", q);
			} else {
				snprintf(mtlpath, sizeof(mtlpath), "%s/%s", objdir, q);
			}

			objReadMtl(m, mtlpath, objdir);
			mtlread = 1;
		}
	}

	fclose(f);

	// Materials named by 'usemtl' lines with no MTL, or added by them after the
	// MTL was read, are classified by name here.
	for (u32 i = 0; i < m->nummaterials; i++) {
		if (m->materials[i].kind == OBJMAT_NONE && m->materials[i].image[0] == '\0') {
			objClassifyMaterial(&m->materials[i], objdir);
		}
	}

	free(src.pos);
	free(src.col);
	free(src.hascol);
	free(src.uv);
	free(src.nrm);
	free(src.keys);

	if (!ok || m->numtris == 0) {
		sysLogPrintf(LOG_ERROR, "objmesh: %s: %s", objpath, ok ? "no faces" : "out of memory");
		objmeshFree(m);
		return NULL;
	}

	if (badfaces) {
		sysLogPrintf(LOG_WARNING, "objmesh: %s: %d faces index vertices the file does not have",
				objpath, badfaces);
	}

	return m;
}

#endif
