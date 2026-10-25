#!/bin/sh
# Full pipeline: lift ON.EXE -> build -> dump patched VGA driver -> lift VGA variant -> rebuild.
# usage: tools/build_all.sh <ON.EXE> <game dir>      (env: PCRECOMP_HOME)
set -e
EXE=$1; DIR=$2
cd "$(dirname "$0")/.."
export PCRECOMP_HOME=${PCRECOMP_HOME:-$PWD/.pcrecomp}
export PYTHONPATH=$PCRECOMP_HOME/tools/disasm:$PCRECOMP_HOME/tools/lift
mkdir -p work
echo "== [1/5] lifting ON.EXE to C"
python tools/lift_full.py "$EXE" | tail -3
echo "== [2/5] building (first pass)"
cmake -S . -B build -G Ninja >/dev/null
cmake --build build
echo "== [3/5] capturing the patched VGA driver"
rm -f work/vga_drv.bin
NEP_VGA=1 NEP_DUMP_VGADRV=work/vga_drv.bin NEP_SECONDS=12 ./build/nep "$EXE" "$DIR" >/dev/null 2>&1 || true
[ -s work/vga_drv.bin ] || { echo "could not capture the VGA driver (work/vga_drv.bin missing)"; exit 1; }
echo "== [4/5] lifting the VGA driver variant"
NEP_VARIANT=vga python tools/lift_full.py "$EXE" | tail -3
echo "== [5/5] building (final)"
cmake -S . -B build -G Ninja >/dev/null
cmake --build build
echo "== done: build/nep"
