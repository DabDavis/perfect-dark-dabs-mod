#include <string.h>
#include <PR/ultratypes.h>
#include "net/netproto.h"
#include "net/netcmd.h"


void netCmdPack(const struct netcmd *c, u32 tick, u8 *p)
{
	const u32 vt = c->viewtick == NETCMD_NOVIEW ? NETCMD_NOVIEW : tick - c->viewtick;
	u32 v;
	s32 i = 0;

	memset(p, 0, NETCMD_PACKED);
#define PUT8(x) p[i++] = (u8)(x)
#define PUT32(x) do { v = (u32)(x); PUT8(v); PUT8(v >> 8); PUT8(v >> 16); PUT8(v >> 24); } while (0)
	PUT32(c->buttons);
	PUT8(c->sx); PUT8(c->sy); PUT8(c->rsx); PUT8(c->rsy);
	memcpy(&v, &c->mdx, 4); PUT32(v);
	memcpy(&v, &c->mdy, 4); PUT32(v);
	PUT8(c->flags);
	PUT32(vt);
	PUT8(c->viewfrac); PUT8(c->viewdelay);
	PUT8(c->equip[0]); PUT8(c->equip[1]);
	PUT8(c->device[0]); PUT8(c->device[1]);
	PUT8(c->body[0]); PUT8(c->body[0] >> 8); PUT8(c->body[1]); PUT8(c->body[1] >> 8);
	PUT8(c->bodyflags);
#undef PUT8
#undef PUT32
}

void netCmdUnpack(const u8 *p, u32 tick, struct netcmd *c)
{
	s32 i = 0;
	u32 v;
	u32 vt;

#define GET8() (p[i++])
#define GET32() (i += 4, (u32)p[i - 4] | ((u32)p[i - 3] << 8) | ((u32)p[i - 2] << 16) | ((u32)p[i - 1] << 24))
	c->buttons = GET32();
	c->sx = (s8)GET8(); c->sy = (s8)GET8(); c->rsx = (s8)GET8(); c->rsy = (s8)GET8();
	v = GET32(); memcpy(&c->mdx, &v, 4);
	v = GET32(); memcpy(&c->mdy, &v, 4);
	c->flags = GET8();
	vt = GET32();
	c->viewtick = vt == NETCMD_NOVIEW ? NETCMD_NOVIEW : tick - vt;
	c->viewfrac = GET8(); c->viewdelay = GET8();
	c->equip[0] = GET8(); c->equip[1] = GET8();
	c->device[0] = GET8(); c->device[1] = GET8();
	c->body[0] = (u16)(p[i] | (p[i + 1] << 8)); i += 2;
	c->body[1] = (u16)(p[i] | (p[i + 1] << 8)); i += 2;
	c->bodyflags = GET8();
#undef GET8
#undef GET32

	// the optional fields mean nothing without their flags
	if (!(c->flags & NETCMD_EQUIP)) {
		c->equip[0] = c->equip[1] = 0;
	}

	if (!(c->flags & NETCMD_DEVICE)) {
		c->device[0] = c->device[1] = 0;
	}

	if (!(c->flags & NETCMD_BODY)) {
		c->body[0] = c->body[1] = 0;
		c->bodyflags = 0;
	}
}

