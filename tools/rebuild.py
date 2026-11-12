"""The build driver under a host-neutral name: tools/rebuild_win.py, which builds on Windows
(clang-cl + lld-link) and on Linux (clang + lld; its step_compile_linux). Same arguments:

    python -I tools/rebuild.py [--only step,step] [--from step]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from rebuild_win import main   # noqa: E402

if __name__ == "__main__":
    main()
