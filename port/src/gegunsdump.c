#include <stdio.h>
#include <string.h>
#include <ultra64.h>
#include <PR/ultratypes.h>
#include "platform.h"
#include "constants.h"
#include "types.h"
#include "data.h"
#include "system.h"
#include "geguns.h"

#ifndef PLATFORM_N64

/**
 * Every field of every GoldenEye weapon definition, as text, one line a field.
 *
 * A pointer is written as what it points at - the numbers under it - and as
 * whose it is: the stock weapon's (by number) whose definition holds the same
 * pointer, or "own" for a copy that belongs to the GoldenEye gun alone. So two
 * dumps diff field by field, and a definition that no longer copies its host
 * shows each thing it used to take from it as a line that changed on purpose.
 *
 * PD_GEGUNS_DUMP=<path> writes one when the definitions are built; from gdb,
 * call gegunsDump("<path>").
 */

static const char *dumpOwner(const void *ptr, s32 kind, s32 f)
{
	static char buf[32];

	if (!ptr) {
		return "null";
	}

	for (s32 w = 1; w < WEAPON_GE_FIRST; w++) {
		const struct weapon *def = g_Weapons[w];

		if (!def) {
			continue;
		}

		switch (kind) {
		case 0: // function
			for (s32 g = 0; g < 2; g++) {
				if (def->functions[g] == ptr) {
					snprintf(buf, sizeof(buf), "stock %02x fn%d", w, g);
					return buf;
				}
			}
			break;
		case 1: // ammo
			for (s32 g = 0; g < 2; g++) {
				if (def->ammos[g] == ptr) {
					snprintf(buf, sizeof(buf), "stock %02x ammo%d", w, g);
					return buf;
				}
			}
			break;
		case 2: // aim
			if (def->aimsettings == ptr) {
				snprintf(buf, sizeof(buf), "stock %02x", w);
				return buf;
			}
			break;
		case 3: // anything a function points at: search the functions' fields
			for (s32 g = 0; g < 2; g++) {
				const struct weaponfunc *fn = def->functions[g];

				if (!fn) {
					continue;
				}

				if (fn->noisesettings == ptr || fn->fire_animation == ptr) {
					snprintf(buf, sizeof(buf), "stock %02x fn%d", w, g);
					return buf;
				}

				if ((fn->type & 0xff) == INVENTORYFUNCTYPE_SHOOT) {
					const struct weaponfunc_shoot *s = (const struct weaponfunc_shoot *)fn;

					if (s->recoilsettings == ptr) {
						snprintf(buf, sizeof(buf), "stock %02x fn%d", w, g);
						return buf;
					}

					if (fn->type == INVENTORYFUNCTYPE_SHOOT_AUTOMATIC) {
						const struct weaponfunc_shootauto *a = (const struct weaponfunc_shootauto *)fn;

						if ((const void *)a->vibrationstart == ptr || (const void *)a->vibrationmax == ptr) {
							snprintf(buf, sizeof(buf), "stock %02x fn%d", w, g);
							return buf;
						}
					}
				}
			}

			for (s32 g = 0; g < 2; g++) {
				if (def->ammos[g] && def->ammos[g]->reload_animation == ptr) {
					snprintf(buf, sizeof(buf), "stock %02x ammo%d", w, g);
					return buf;
				}
			}

			if (def->equip_animation == ptr || def->unequip_animation == ptr
					|| def->pritosec_animation == ptr || def->sectopri_animation == ptr
					|| def->gunviscmds == ptr || def->partvisibility == ptr) {
				snprintf(buf, sizeof(buf), "stock %02x", w);
				return buf;
			}
			break;
		}
	}

	(void)f;
	return "own";
}

static void dumpGuncmd(FILE *fp, const char *what, const struct guncmd *cmd)
{
	fprintf(fp, "  %s: %s", what, dumpOwner(cmd, 3, 0));

	for (s32 n = 0; cmd && n < 64; n++, cmd++) {
		fprintf(fp, " [%d %d %d %ld]", cmd->type, cmd->unk01, cmd->unk02, (long)cmd->unk04);

		if (cmd->type == 0) { // GUNCMD_END
			break;
		}
	}

	fprintf(fp, "\n");
}

static void dumpFunc(FILE *fp, s32 f, const struct weaponfunc *fn)
{
	fprintf(fp, " fn%d: %s\n", f, dumpOwner(fn, 0, f));

	if (!fn) {
		return;
	}

	fprintf(fp, "  type %04x name %d ammoindex %d flags %08x\n", fn->type, fn->name, fn->ammoindex, fn->flags);

	if (fn->noisesettings) {
		const struct noisesettings *n = fn->noisesettings;
		fprintf(fp, "  noise: %s %g %g %g %g %g\n", dumpOwner(n, 3, 0),
				n->minradius, n->maxradius, n->incradius, n->decbasespeed, n->decremspeed);
	} else {
		fprintf(fp, "  noise: null\n");
	}

	dumpGuncmd(fp, "fire_animation", fn->fire_animation);

	switch (fn->type & 0xff) {
	case INVENTORYFUNCTYPE_SHOOT: {
		const struct weaponfunc_shoot *s = (const struct weaponfunc_shoot *)fn;

		if (s->recoilsettings) {
			fprintf(fp, "  recoilsettings: %s %g %g %g %g %d\n", dumpOwner(s->recoilsettings, 3, 0),
					s->recoilsettings->xrange, s->recoilsettings->yrange, s->recoilsettings->zrange,
					s->recoilsettings->unk0c, s->recoilsettings->unk10);
		} else {
			fprintf(fp, "  recoilsettings: null\n");
		}

		fprintf(fp, "  recoverytime60 %d damage %g spread %g speeds %d %d %d %d\n",
				s->recoverytime60, s->damage, s->spread, s->unk24, s->unk25, s->unk26, s->unk27);
		fprintf(fp, "  recoildist %g recoilangle %g slidemax %g impactforce %g duration60 %d shootsound %d penetration %d\n",
				s->recoildist, s->recoilangle, s->slidemax, s->impactforce, s->duration60, s->shootsound, s->penetration);

		if (fn->type == INVENTORYFUNCTYPE_SHOOT_AUTOMATIC) {
			const struct weaponfunc_shootauto *a = (const struct weaponfunc_shootauto *)fn;

			fprintf(fp, "  initialrpm %g maxrpm %g turretaccel %d turretdecel %d\n",
					a->initialrpm, a->maxrpm, a->turretaccel, a->turretdecel);
			fprintf(fp, "  vibrationstart %s", dumpOwner(a->vibrationstart, 3, 0));
			for (s32 k = 0; a->vibrationstart && k < 4; k++) {
				fprintf(fp, " %g", a->vibrationstart[k]);
			}
			fprintf(fp, "\n  vibrationmax %s", dumpOwner(a->vibrationmax, 3, 0));
			for (s32 k = 0; a->vibrationmax && k < 4; k++) {
				fprintf(fp, " %g", a->vibrationmax[k]);
			}
			fprintf(fp, "\n");
		} else if (fn->type == INVENTORYFUNCTYPE_SHOOT_PROJECTILE) {
			const struct weaponfunc_shootprojectile *p = (const struct weaponfunc_shootprojectile *)fn;

			fprintf(fp, "  projectile model %d scale %g speed %d unk50 %g traveldist %d timer60 %d reflectangle %g soundnum %d\n",
					p->projectilemodelnum, p->scale, p->speed, p->unk50, p->traveldist, p->timer60,
					p->reflectangle, p->soundnum);
		}
		break;
	}
	case INVENTORYFUNCTYPE_THROW: {
		const struct weaponfunc_throw *t = (const struct weaponfunc_throw *)fn;
		fprintf(fp, "  throw model %d activatetime60 %d recoverytime60 %d damage %g\n",
				t->projectilemodelnum, t->activatetime60, t->recoverytime60, t->damage);
		break;
	}
	case INVENTORYFUNCTYPE_MELEE: {
		const struct weaponfunc_melee *m = (const struct weaponfunc_melee *)fn;
		fprintf(fp, "  melee damage %g range %g\n", m->damage, m->range);
		break;
	}
	case INVENTORYFUNCTYPE_SPECIAL: {
		const struct weaponfunc_special *s = (const struct weaponfunc_special *)fn;
		fprintf(fp, "  special %d recoverytime60 %d soundnum %d\n", s->specialfunc, s->recoverytime60, s->soundnum);
		break;
	}
	case INVENTORYFUNCTYPE_DEVICE: {
		const struct weaponfunc_device *d = (const struct weaponfunc_device *)fn;
		fprintf(fp, "  device %08x\n", d->device);
		break;
	}
	}
}

void gegunsDump(const char *path)
{
	FILE *fp = fopen(path, "w");

	if (!fp) {
		sysLogPrintf(LOG_WARNING, "geguns: cannot write the dump to %s", path);
		return;
	}

	for (s32 i = 0; i < NUM_GE_WEAPONS; i++) {
		const struct weapon *def = &g_GeWeaponDefs[i];

		fprintf(fp, "weapon %02x (host %02x)%s\n", WEAPON_GE_FIRST + i, g_GeWeaponHosts[i],
				gegunsIsBorrowed(i) ? " borrowed" : "");
		fprintf(fp, " hi_model %d lo_model %d\n", def->hi_model, def->lo_model);
		dumpGuncmd(fp, "equip", def->equip_animation);
		dumpGuncmd(fp, "unequip", def->unequip_animation);
		dumpGuncmd(fp, "pritosec", def->pritosec_animation);
		dumpGuncmd(fp, "sectopri", def->sectopri_animation);

		for (s32 f = 0; f < 2; f++) {
			dumpFunc(fp, f, def->functions[f]);
		}

		for (s32 a = 0; a < 2; a++) {
			const struct inventory_ammo *am = def->ammos[a];

			fprintf(fp, " ammo%d: %s", a, dumpOwner(am, 1, a));

			if (am) {
				fprintf(fp, " type %d casingeject %d clipsize %d flags %02x\n",
						am->type, am->casingeject, am->clipsize, am->flags);
				dumpGuncmd(fp, "reload", am->reload_animation);
			} else {
				fprintf(fp, "\n");
			}
		}

		if (def->aimsettings) {
			const struct invaimsettings *aim = def->aimsettings;
			fprintf(fp, " aim: %s zoomfov %g up %g down %g side %g damppal %g damp %g tracktype %d flags %08x\n",
					dumpOwner(aim, 2, 0), aim->zoomfov, aim->guntransup, aim->guntransdown, aim->guntransside,
					aim->aimdamppal, aim->aimdamp, aim->tracktype, aim->flags);
		} else {
			fprintf(fp, " aim: null\n");
		}

		fprintf(fp, " muzzlez %g pos %g %g %g sway %g\n", def->muzzlez, def->posx, def->posy, def->posz, def->sway);
		fprintf(fp, " gunviscmds: %s\n", dumpOwner(def->gunviscmds, 3, 0));
		fprintf(fp, " partvisibility: %s", dumpOwner(def->partvisibility, 3, 0));

		for (const struct modelpartvisibility *p = def->partvisibility; p && p->part != 0xff; p++) {
			fprintf(fp, " %d:%d", p->part, p->visible);
		}

		fprintf(fp, "\n names %d %d manufacturer %d description %d\n",
				def->shortname, def->name, def->manufacturer, def->description);
		fprintf(fp, " flags %08x flags2 %08x flags3 %08x unequippedreloadindex %d pickupsound %d\n",
				def->flags, def->flags2, def->flags3, def->unequippedreloadindex, def->pickupsound);
	}

	fclose(fp);
	sysLogPrintf(LOG_NOTE, "geguns: definitions dumped to %s", path);
}

#endif
