#!/usr/bin/env python3
"""Propose English (UK) candidates from the US English (lang/en-GB).

Scans Perfect Dark's banks (src/assets/ntsc-final/lang/*.json, the `en` field
only), lang/_source/port.json and lang/_source/ge.json (run extract.py first)
for the US spellings in WORDS and prints each string with its British form.
The output is a proposal to review by hand, not a pack: some hits are names,
code words or other senses (a computer program stays "program", the verb
"license" stays).

  python3 tools/langpack/engb.py            # list every candidate
  python3 tools/langpack/engb.py --json DIR # write proposals as DIR/{pd/<bank>,port,ge}.json
"""

import argparse
import glob
import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# US -> UK, whole words, matched case-insensitively and re-cased like the
# original (lower, Capital, UPPER).
WORDS = {
    # -our
    'armor': 'armour', 'armored': 'armoured', 'armory': 'armoury',
    'color': 'colour', 'colors': 'colours', 'colored': 'coloured',
    'colorful': 'colourful',
    'honor': 'honour', 'honors': 'honours', 'honored': 'honoured',
    'honorable': 'honourable', 'dishonored': 'dishonoured',
    'humor': 'humour',
    'rumor': 'rumour', 'rumors': 'rumours', 'rumored': 'rumoured',
    'enamored': 'enamoured', 'favor': 'favour', 'favorite': 'favourite',
    'favorites': 'favourites', 'behavior': 'behaviour', 'neighbor': 'neighbour',
    'vapor': 'vapour', 'labor': 'labour', 'harbor': 'harbour',
    'endeavor': 'endeavour', 'valor': 'valour', 'savior': 'saviour',
    # -re
    'center': 'centre', 'centers': 'centres', 'centered': 'centred',
    'meter': 'metre', 'meters': 'metres', 'kilometer': 'kilometre',
    'kilometers': 'kilometres', 'theater': 'theatre', 'fiber': 'fibre',
    'caliber': 'calibre', 'somber': 'sombre', 'specter': 'spectre',
    # -ence
    'defense': 'defence', 'defenses': 'defences', 'offense': 'offence',
    'license': 'licence',  # noun only
    # -ise / -yse
    'acclimatize': 'acclimatise', 'analyze': 'analyse', 'analyzing': 'analysing',
    'analyzed': 'analysed', 'authorized': 'authorised',
    'unauthorized': 'unauthorised', 'authorization': 'authorisation',
    'customize': 'customise', 'customized': 'customised',
    'customizing': 'customising', 'familiarize': 'familiarise',
    'immobilized': 'immobilised', 'jeopardize': 'jeopardise',
    'mechanized': 'mechanised', 'minimize': 'minimise', 'minimized': 'minimised',
    'maximize': 'maximise', 'neutralize': 'neutralise',
    'neutralized': 'neutralised', 'optimized': 'optimised',
    'optimize': 'optimise', 'organized': 'organised', 'organize': 'organise',
    'polarized': 'polarised', 'randomize': 'randomise',
    'randomized': 'randomised', 'realize': 'realise', 'realized': 'realised',
    'recognize': 'recognise', 'recognized': 'recognised',
    'specialized': 'specialised', 'stabilized': 'stabilised',
    'synchronize': 'synchronise', 'synchronized': 'synchronised',
    'apologize': 'apologise', 'utilize': 'utilise', 'finalize': 'finalise',
    'initialize': 'initialise', 'initialized': 'initialised',
    'prioritize': 'prioritise', 'paralyzed': 'paralysed',
    'tranquilizer': 'tranquiliser', 'analyzer': 'analyser',
    'randomizer': 'randomiser',
    # -ll-
    'traveled': 'travelled', 'traveling': 'travelling', 'traveler': 'traveller',
    'canceled': 'cancelled', 'canceling': 'cancelling', 'labeled': 'labelled',
    'labeling': 'labelling', 'modeled': 'modelled', 'modeling': 'modelling',
    'fueled': 'fuelled', 'signaled': 'signalled', 'leveled': 'levelled',
    'totaled': 'totalled', 'marvelous': 'marvellous', 'fulfill': 'fulfil',
    'enrollment': 'enrolment', 'skillful': 'skilful',
    # others
    'gray': 'grey', 'catalog': 'catalogue', 'maneuver': 'manoeuvre',
    'maneuvers': 'manoeuvres', 'maneuvering': 'manoeuvring',
    'aluminum': 'aluminium', 'tire': 'tyre', 'tires': 'tyres', 'curb': 'kerb',
    'practicing': 'practising', 'practiced': 'practised',
    'judgment': 'judgement', 'aging': 'ageing', 'sulfur': 'sulphur',
    'program': 'programme',  # broadcast/plan only - not software
    'programs': 'programmes',
    # vocabulary
    'elevator': 'lift', 'elevators': 'lifts', 'trash': 'rubbish',
    'flashlight': 'torch', 'flashlights': 'torches', 'airplane': 'aeroplane',
    'garbage': 'rubbish',
    'analog': 'analogue', 'checkered': 'chequered',
}

# Hits kept as the US English has them, by (key, US word) - the review's
# verdicts. Dr. Caroll's "program" is software; "Catalog Inc." is a company;
# "Elevator Music" is a credit's joke title.
EXCEPT = {
    ('L_PAM_039', 'program'), ('L_PAM_040', 'program'), ('L_PAM_042', 'program'),
    ('ge.title.18', 'Catalog'), ('ge.len.62', 'Elevator'),
}

PAT = re.compile(r'\b(' + '|'.join(sorted(WORDS, key=len, reverse=True)) + r')\b', re.I)


def recase(src, dst):
    if src.isupper() and len(src) > 1:
        return dst.upper()
    if src[0].isupper():
        return dst[0].upper() + dst[1:]
    return dst


def british(text, key=None):
    def sub(m):
        if (key, m.group(0)) in EXCEPT:
            return m.group(0)
        return recase(m.group(0), WORDS[m.group(0).lower()])
    return PAT.sub(sub, text)


def sources():
    """(catalog, key, English) for every string of the three sources."""
    for path in sorted(glob.glob(os.path.join(ROOT, 'src/assets/ntsc-final/lang/*.json'))):
        bank = os.path.splitext(os.path.basename(path))[0]
        with open(path, encoding='utf-8') as f:
            for row in json.load(f):
                if row.get('en'):
                    yield 'pd/' + bank, row['id'], row['en']
    for name in ('port', 'ge'):
        path = os.path.join(ROOT, 'lang/_source', name + '.json')
        if not os.path.exists(path):
            raise SystemExit('%s missing: run tools/langpack/extract.py' % path)
        with open(path, encoding='utf-8') as f:
            for key, text in json.load(f).items():
                yield name, key, text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--json', metavar='DIR', help='write proposals as pack files here')
    args = ap.parse_args()

    out = {}
    for cat, key, text in sources():
        uk = british(text, key)
        if uk == text:
            continue
        out.setdefault(cat, {})[key] = uk
        if not args.json:
            words = sorted(set(m.group(0) for m in PAT.finditer(text)))
            print('%s %s [%s]\n  US: %r\n  UK: %r' % (cat, key, ', '.join(words), text, uk))

    if args.json:
        for cat, strings in out.items():
            path = os.path.join(args.json, cat + '.json')
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, 'w', encoding='utf-8') as f:
                json.dump(strings, f, ensure_ascii=False, indent=1)
                f.write('\n')
    for cat in sorted(out):
        print('%-10s %d' % (cat, len(out[cat])), file=sys.stderr)


if __name__ == '__main__':
    main()
