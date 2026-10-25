"""Run pcrecomp's 16-bit function analysis on ..\\ON.EXE.

Usage:  set PCRECOMP=C:\\src\\pcrecomp  &&  python tools\\analyze.py
"""
import os
import subprocess
import sys
from pathlib import Path

root = Path(__file__).resolve().parent.parent
exe = root.parent / "ON.EXE"
pcrecomp = os.environ.get("PCRECOMP")
if not pcrecomp:
    sys.exit("Set PCRECOMP to your pcrecomp checkout")
disasm = Path(pcrecomp) / "tools" / "disasm"

env = dict(os.environ, PYTHONPATH=str(disasm))
(root / "work").mkdir(exist_ok=True)
subprocess.run(
    [sys.executable, str(disasm / "analyze.py"), str(exe),
     "-symbols", str(root / "work" / "symbols.toml")],
    env=env, check=True)
