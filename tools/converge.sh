#!/bin/sh
# Re-lift -> build -> run until the runtime stops finding new dispatch targets.
# usage: tools/converge.sh <ON.EXE> <game dir> [rounds]   (env: PCRECOMP_HOME, NEP_KEYS, NEP_SECONDS)
EXE=$1; DIR=$2; N=${3:-6}
export PYTHONPATH=${PCRECOMP_HOME:-../pcrecomp}/tools/disasm:${PCRECOMP_HOME:-../pcrecomp}/tools/lift
mkdir -p work
for i in $(seq 1 "$N"); do
  before=$(sort -u work/nep_misses.txt 2>/dev/null | wc -l)
  python3 tools/lift_full.py "$EXE" | grep -E "feedback|lifted"
  cmake -S . -B build >/dev/null && cmake --build build -j8 2>&1 | grep -E "error|undefined"
  ./build/nep "$EXE" "$DIR" >/dev/null 2>&1
  after=$(sort -u work/nep_misses.txt 2>/dev/null | wc -l)
  echo "round $i: misses $before -> $after"
  [ "$before" = "$after" ] && break
done
