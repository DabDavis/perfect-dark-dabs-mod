"""GoldenEye's twenty-five guns: GoldenEye's ITEM_ number, a name, and our
weapon number (gegunsItemNumber() in port/src/geguns.c, read the other way)."""

GUNS = [
    # item, key,            our weapon
    (4,  'pp7',          0x5e),
    (5,  'pp7silenced',  0x5f),
    (6,  'dd44',         0x60),
    (7,  'klobb',        0x61),
    (8,  'kf7',          0x62),
    (9,  'zmg',          0x63),
    (10, 'd5k',          0x64),
    (11, 'd5ksilenced',  0x65),
    (12, 'phantom',      0x66),
    (13, 'ar33',         0x67),
    (14, 'rcp90',        0x68),
    (15, 'shotgun',      0x69),
    (16, 'autoshotgun',  0x6a),
    (17, 'sniper',       0x6b),
    (18, 'cougar',       0x6c),
    (19, 'goldengun',    0x6d),
    (22, 'moonraker',    0x6e),
    (24, 'grenadelauncher', 0x6f),
    (25, 'rocketlauncher',  0x70),
    (2,  'huntingknife', 0x71),
    (3,  'throwingknife', 0x72),
    (26, 'grenade',      0x73),
    (27, 'timedmine',    0x74),
    (28, 'proximitymine', 0x75),
    (29, 'remotemine',   0x76),
]


def gun(key):
    k = str(key).lower()
    for g in GUNS:
        if k in (g[1], str(g[0])):
            return g
    raise KeyError('no gun %r; one of %s' % (key, ' '.join(g[1] for g in GUNS)))
