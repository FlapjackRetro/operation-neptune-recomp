#!/bin/sh
# Re-lift -> build -> run until the runtime stops finding new dispatch targets.
# usage: tools/converge.sh <ON.EXE> <game dir> [rounds]   (env: PCRECOMP_HOME, NEP_KEYS, NEP_SECONDS, NEP_VGA)
# With NEP_VGA set the run takes the VGA path, and the patched VGA driver
# (work/vga_drv.bin, dumped by the first VGA run) is lifted as a variant.
EXE=$1; DIR=$2; N=${3:-6}
export PYTHONPATH=${PCRECOMP_HOME:-../pcrecomp}/tools/disasm:${PCRECOMP_HOME:-../pcrecomp}/tools/lift
mkdir -p work
cnt() { cat work/nep_misses.txt work/nep_misses_vga.txt 2>/dev/null | sort -u | wc -l; }
if [ -n "$NEP_VGA" ] && [ ! -f work/vga_drv.bin ] && [ -x build/nep ]; then
  NEP_DUMP_VGADRV=work/vga_drv.bin ./build/nep "$EXE" "$DIR" >/dev/null 2>&1   # patched driver bytes
fi
for i in $(seq 1 "$N"); do
  before=$(cnt)
  python3 tools/lift_full.py "$EXE" | grep -E "feedback|lifted"
  if [ -n "$NEP_VGA" ] && [ -f work/vga_drv.bin ]; then
    NEP_VARIANT=vga python3 tools/lift_full.py "$EXE" | grep -E "feedback|variant"
  fi
  cmake -S . -B build >/dev/null && cmake --build build -j8 2>&1 | grep -E "error|undefined"
  ./build/nep "$EXE" "$DIR" >/dev/null 2>&1
  after=$(cnt)
  echo "round $i: misses $before -> $after"
  [ "$before" = "$after" ] && break
done
