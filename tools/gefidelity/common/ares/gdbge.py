"""On the ares oracle, a scenario's `import gdbge` gets aresge: twin.py points
GF_COMMON here, so world/dump.py and the like run unchanged on the cartridge."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from aresge import *  # noqa: F401,F403
