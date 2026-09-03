#!/usr/bin/env python3

"""
Upscales dumped textures into a texture pack.

--dump takes either kind of dump the game writes.

A dump made while playing holds what was drawn, as <texnum>.png; with a pack
already selected it holds only what that pack does not cover, so pointing this
at one fills in exactly the gaps. A dump made with --dump-textures holds every
texture in the ROM as raw texels and a manifest, which this converts itself -
that is the way to upscale the whole catalogue without visiting every room.

Output goes to <out>/textures/, which is what the game reads. Filenames are kept
as they came, because the loader takes <texnum> followed by anything.

Upscaling is done by realesrgan-ncnn-vulkan against Upscayl's models. Neither is
vendored here: point --models at an Upscayl checkout (its resources/models) and
have the binary on PATH.

    tools/texpack/upscale.py \\
        --dump ~/.local/share/perfectdark/texturedump/ntsc-final \\
        --out  ~/.local/share/perfectdark/texture-packs/MyPack \\
        --models ~/upscayl/resources/models

Textures are padded by wrapping their own edges before upscaling and cropped
back afterwards. Most game textures tile, and without that the upscaler invents
different detail along each edge, which shows up as a seam on every wall.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import n64tex

# A dump is named <texnum> followed by anything, four lowercase hex digits.
NAME_RE = re.compile(r'^([0-9a-f]{4})(?:_[^.]*)?\.png$', re.IGNORECASE)

# Below this, on either side, an upscaler has nothing to work with: a 2x1 strip
# of gradient comes back as invented noise. Left alone, so the game keeps
# drawing the original.
MIN_SIZE = 8

UPSCALER = 'realesrgan-ncnn-vulkan'


def need_pillow():
    try:
        from PIL import Image
    except ImportError:
        sys.exit('Pillow is needed (pip install pillow).')
    return Image


def find_models(path):
    if not os.path.isdir(path):
        sys.exit(f'{path} is not a directory - point --models at an Upscayl '
                 f'checkout\'s resources/models')

    names = sorted(os.path.splitext(f)[0] for f in os.listdir(path) if f.endswith('.param'))

    if not names:
        sys.exit(f'no models in {path}')

    return names


def pad_wrapped(im, pad):
    """
    Surrounds an image with copies of its own opposite edges.

    A tiling texture's left edge continues into its right, so this gives the
    upscaler the context it would have had in the game and the result still
    meets itself. The padding is cropped off afterwards.
    """
    Image = need_pillow()
    w, h = im.size
    out = Image.new(im.mode, (w + pad * 2, h + pad * 2))

    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            out.paste(im, (pad + dx * w, pad + dy * h))

    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
            formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dump', required=True, help='a texturedump/<romid> directory')
    ap.add_argument('--out', required=True, help='pack directory to write (textures/ inside it)')
    ap.add_argument('--models', default=os.path.expanduser('~/upscayl/resources/models'),
            help="Upscayl's resources/models (default: ~/upscayl/resources/models)")
    ap.add_argument('--model', default='upscayl-standard-4x')
    ap.add_argument('--scale', type=int, default=4, choices=(2, 3, 4))
    ap.add_argument('--pad', type=int, default=8,
            help='texels of wrapped padding for context, 0 to disable (default 8)')
    ap.add_argument('--gpu', default='0', help='vulkan device id, or -1 for CPU (default 0)')
    ap.add_argument('--min-size', type=int, default=MIN_SIZE,
            help=f'skip textures smaller than this on either side (default {MIN_SIZE})')
    ap.add_argument('--limit', type=int, default=0, help='only do this many, for a trial run')
    ap.add_argument('--list-models', action='store_true')
    args = ap.parse_args()

    models = find_models(args.models)

    if args.list_models:
        print('\n'.join(models))
        return

    if args.model not in models:
        sys.exit(f'no model called {args.model}. Available:\n  ' + '\n  '.join(models))

    if not shutil.which(UPSCALER):
        sys.exit(f'{UPSCALER} is not on PATH. It is the engine Upscayl drives; '
                 f'install it or add it to PATH.')

    Image = need_pillow()

    manifest = os.path.join(args.dump, 'manifest.csv')
    from_raw = os.path.exists(manifest)

    if from_raw:
        print(f'reading the whole texture table from {os.path.basename(manifest)}')
    else:
        sources = [n for n in sorted(os.listdir(args.dump)) if NAME_RE.match(n)]

        if not sources:
            sys.exit(f'no <texnum>.png files and no manifest.csv in {args.dump}')

    outdir = os.path.join(args.out, 'textures')
    os.makedirs(outdir, exist_ok=True)

    with tempfile.TemporaryDirectory(prefix='pdupscale-') as tmp:
        indir = os.path.join(tmp, 'in')
        updir = os.path.join(tmp, 'out')
        os.makedirs(indir)
        os.makedirs(updir)

        sizes = {}
        skipped = 0

        if from_raw:
            def images():
                for _texnum, name, w, h, rgba in n64tex.load_dump(args.dump):
                    yield name, Image.frombytes('RGBA', (w, h), bytes(rgba))
        else:
            def images():
                for name in sources:
                    yield name, Image.open(os.path.join(args.dump, name)).convert('RGBA')

        for name, im in images():
            if min(im.size) < args.min_size:
                skipped += 1
                continue

            sizes[name] = im.size
            padded = pad_wrapped(im, args.pad) if args.pad else im
            padded.save(os.path.join(indir, name))

            if args.limit and len(sizes) >= args.limit:
                break

        if not sizes:
            sys.exit('nothing big enough to upscale')

        print(f'{len(sizes)} textures to upscale'
              + (f', {skipped} left alone as too small' if skipped else '')
              + f' ({args.model}, {args.scale}x)')

        # One process over the whole directory: spawning it per file costs more
        # than the upscaling does at these sizes.
        cmd = [UPSCALER, '-i', indir, '-o', updir, '-m', args.models,
               '-n', args.model, '-s', str(args.scale), '-g', args.gpu, '-f', 'png']
        result = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)

        if result.returncode != 0:
            sys.exit(f'{UPSCALER} failed:\n'
                     + result.stderr.decode(errors='replace').strip()[-2000:])

        written = 0

        for name, (w, h) in sizes.items():
            src = os.path.join(updir, name)

            if not os.path.exists(src):
                continue

            im = Image.open(src).convert('RGBA')

            if args.pad:
                p = args.pad * args.scale
                im = im.crop((p, p, p + w * args.scale, p + h * args.scale))

            im.save(os.path.join(outdir, name))
            written += 1

        print(f'wrote {written} textures to {outdir}')

        if written < len(sizes):
            print(f'{len(sizes) - written} came back missing from the upscaler')


if __name__ == '__main__':
    main()
