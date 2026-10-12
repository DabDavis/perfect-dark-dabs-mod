#ifndef _IN_KILLFEED_H
#define _IN_KILLFEED_H

#include <ultra64.h>
#include <PR/ultratypes.h>

/**
 * The kill feed (port/src/killfeed.c, Mod.KillFeed): "Killer [Weapon] Victim"
 * at the top right of a Combat Simulator or GoldenEye match, the last few
 * kills, each fading after about five seconds. Drawing only: what it keeps
 * is read by nothing but its own render, so a replay plays the same game.
 *
 * Mod.KillFeed: KILLFEED_AUTO (-1, the default) is on online and off
 * offline; KILLFEED_OFF (0) and KILLFEED_ON (1) are that everywhere.
 */

#define KILLFEED_AUTO -1
#define KILLFEED_OFF   0
#define KILLFEED_ON    1

// What a kill was made with, besides a weapon number: an explosion nothing
// named (a barrel, a vehicle), and nothing known. As DEATH carries it (u8).
#define KILLFEED_WEAPON_EXPLOSION 0xfe
#define KILLFEED_WEAPON_NONE      0xff

struct chrdata;
struct gset;
struct prop;

// Mod.KillFeed as it stands here (offline/online resolved)
s32 killfeedIsOn(void);

// chrDamage(), past its early returns: the weapon of the latest hit on chr
// (gset NULL for an explosion's, then the explosion's own weapon), resolved
// against what the attacker aprop holds (a rocket is its launcher's)
void killfeedNoteHit(struct chrdata *chr, struct gset *gset, struct prop *aprop, s32 explosion);

// explosionCreate(): what explosion `index` was made by (a projectile's or a
// mine's weapon), and around explosionsTick()'s chrDamageByExplosion() the
// explosion doing the damage (-1 after)
void killfeedNoteExplosion(s32 index, struct prop *source);
void killfeedSetExplosion(s32 index);

// The weapon of a death between these mpchr player numbers, for the wire
// (netEvRecordDeath): KILLFEED_WEAPON_* or a weapon number
u8 killfeedDeathWeapon(s32 aplayernum, s32 vplayernum);

// A client applying the host's DEATH: the weapon it carried, for the
// mpstatsRecordDeath() that follows (one use; -1 clears it after)
void killfeedSetWireWeapon(s32 weapon);

// mpstatsRecordDeath(), once the death counts on this machine
void killfeedRecord(s32 aplayernum, s32 vplayernum);

// lv.c, over the current player's view (a match only)
Gfx *killfeedRender(Gfx *gdl);

#endif
