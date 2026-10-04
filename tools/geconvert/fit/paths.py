"""Where the fit scripts find their inputs, and where the generators write.

Every location outside this directory comes from here, so nothing names a home
directory. Each one can be moved with an environment variable:

    GEFIT_BEAN        the unpacked GoldenEye XBLA (Project Bean) release, the
                      directory holding files/ (files/new, files/original)
                      default: <repo>/../.xbla-work/ge-bean/Bean
    GEFIT_GE_DECOMP   the GoldenEye decomp tree (assets/obseg/..., src/)
                      default: ~/claude-007/007
    GEFIT_GEX_MOD     GoldenEye X 6a as the port's importer unpacked it
                      (files/, segs/data) - only the retired GE-X fits read it
                      default: <repo>/build/mods/GE-X_6a_01-19-25
    GEFIT_GEX_FILES   GE-X's model files (compressed or inflated both read)
                      default: $GEFIT_GEX_MOD/files
    GEFIT_GEX_TEXDUMP a --dump-textures run with GE-X mounted
                      (texture-dumps/pd-n64), for texcompare.py only
                      default: <repo>/build/texture-dumps/pd-n64, or the
                      older layout's texture-dumps/ntsc-final when only
                      that is there

The GoldenEye ROM itself is found by tools/geconvert/gefiles.py (GE_ROM).

Importing this module also puts this directory and tools/geconvert on sys.path.
"""
import atexit
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
GECONVERT = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(GECONVERT))


def _env(name, default):
    return os.path.abspath(os.path.expanduser(os.environ.get(name) or default))


BEAN = _env('GEFIT_BEAN', os.path.join(REPO, '..', '.xbla-work', 'ge-bean', 'Bean'))
GE_DECOMP = _env('GEFIT_GE_DECOMP', '~/claude-007/007')
GEX_MOD = _env('GEFIT_GEX_MOD', os.path.join(REPO, 'build', 'mods', 'GE-X_6a_01-19-25'))
GEX_FILES = _env('GEFIT_GEX_FILES', os.path.join(GEX_MOD, 'files'))
_TEXDUMPS = os.path.join(REPO, 'build', 'texture-dumps')
GEX_TEXDUMP = _env('GEFIT_GEX_TEXDUMP', os.path.join(_TEXDUMPS, 'pd-n64')
                   if os.path.isdir(os.path.join(_TEXDUMPS, 'pd-n64'))
                   or not os.path.isdir(os.path.join(_TEXDUMPS, 'ntsc-final'))
                   else os.path.join(_TEXDUMPS, 'ntsc-final'))
TEXPACK = os.path.join(REPO, 'tools', 'texpack')

for _p in (GECONVERT, HERE):
    if _p not in sys.path:
        sys.path.insert(0, _p)


def data(name):
    """A JSON/text input kept in this directory."""
    return os.path.join(HERE, name)


def repo(*parts):
    return os.path.join(REPO, *parts)


def output(default):
    """The file a generator writes: `-o PATH` on the command line (taken out of
    sys.argv), `-o -` for stdout, else `default` (the real header in the repo).
    Returns an open text file; a real file is closed at exit."""
    path = default
    if '-o' in sys.argv[1:]:
        i = sys.argv.index('-o')
        path = sys.argv[i + 1]
        del sys.argv[i:i + 2]
    if path == '-':
        return sys.stdout
    f = open(path, 'w', newline='\n')
    atexit.register(f.close)
    return f
