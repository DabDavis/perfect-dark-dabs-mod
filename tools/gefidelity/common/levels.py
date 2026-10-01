"""GoldenEye's twenty solo missions, as both games name them.

`index` is GoldenEye's mission number and what Perfect Dark's
`--boot-ge-mission N` takes (0-based). `levelid` is the GoldenEye decomp's
LEVELID_* enum, which the oracle swaps in at bossSetLoadedStage.
"""

MISSIONS = [
    # index, key,        title,        GoldenEye LEVELID
    (0,  'dam',       'Dam',        'LEVELID_DAM'),
    (1,  'facility',  'Facility',   'LEVELID_FACILITY'),
    (2,  'runway',    'Runway',     'LEVELID_RUNWAY'),
    (3,  'surface',   'Surface',    'LEVELID_SURFACE'),
    (4,  'bunker',    'Bunker',     'LEVELID_BUNKER1'),
    (5,  'silo',      'Silo',       'LEVELID_SILO'),
    (6,  'frigate',   'Frigate',    'LEVELID_FRIGATE'),
    (7,  'surface2',  'Surface 2',  'LEVELID_SURFACE2'),
    (8,  'bunker2',   'Bunker 2',   'LEVELID_BUNKER2'),
    (9,  'statue',    'Statue',     'LEVELID_STATUE'),
    (10, 'archives',  'Archives',   'LEVELID_ARCHIVES'),
    (11, 'streets',   'Streets',    'LEVELID_STREETS'),
    (12, 'depot',     'Depot',      'LEVELID_DEPOT'),
    (13, 'train',     'Train',      'LEVELID_TRAIN'),
    (14, 'jungle',    'Jungle',     'LEVELID_JUNGLE'),
    (15, 'control',   'Control',    'LEVELID_CONTROL'),
    (16, 'caverns',   'Caverns',    'LEVELID_CAVERNS'),
    (17, 'cradle',    'Cradle',     'LEVELID_CRADLE'),
    (18, 'aztec',     'Aztec',      'LEVELID_AZTEC'),
    (19, 'egyptian',  'Egyptian',   'LEVELID_EGYPT'),
]

# GoldenEye's DIFFICULTY_* and Perfect Dark's DIFF_* agree number for number.
DIFFICULTIES = {'agent': 0, 'secret': 1, '00': 2, '007': 3}


def mission(key):
    """A mission by key, title or number."""
    k = str(key).strip().lower()
    for m in MISSIONS:
        if k in (str(m[0]), m[1], m[2].lower()):
            return m
    raise KeyError('no mission %r; one of %s' % (key, ' '.join(m[1] for m in MISSIONS)))


def difficulty(key):
    k = str(key).strip().lower()
    if k in DIFFICULTIES:
        return DIFFICULTIES[k]
    if k.isdigit() and int(k) in DIFFICULTIES.values():
        return int(k)
    raise KeyError('no difficulty %r; one of %s' % (key, ' '.join(DIFFICULTIES)))
