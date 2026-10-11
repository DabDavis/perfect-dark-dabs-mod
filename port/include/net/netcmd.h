#ifndef _IN_NET_NETCMD_H
#define _IN_NET_NETCMD_H

#include <PR/ultratypes.h>

/**
 * A client's usercmd (netplayers.c) and its wire form (protocol 30): every
 * field packed to NETCMD_PACKED bytes, the optional ones 0 where their flag
 * is clear and viewtick as the command's own tick less it, so that each is
 * XOR-delta'd (netdelta.c) against the command before it in its CMD, and
 * the first against the newest one the host acked (netproto.h, CMD)
 */
struct netcmd {
	u32 buttons;
	s8 sx;
	s8 sy;
	s8 rsx;
	s8 rsy;
	f32 mdx;
	f32 mdy;
	u8 flags;
	u32 viewtick;            // the host tick the client's puppets were drawn at
	u8 viewfrac;             // ... and the fraction past it, in 1/256ths (NETCMD_NOVIEW: none)
	u8 viewdelay;            // how far its render clock sat behind the newest snapshot, 1/8 ticks
	u8 equip[2];             // NETCMD_EQUIP: the guns a menu put in the hands, right and left (0 none)
	u8 device[2];            // NETCMD_DEVICE: a device's weapon number, and on or off
	u16 body[2];             // NETCMD_BODY: the tethered body's facing and travel, 65536ths of a turn
	u8 bodyflags;            // ... NETBODY_TRAVEL
};

#define NETCMD_NOVIEW 0xffffffff
#define NETCMD_PACKED 32

void netCmdPack(const struct netcmd *c, u32 tick, u8 *p);
void netCmdUnpack(const u8 *p, u32 tick, struct netcmd *c);

#endif
