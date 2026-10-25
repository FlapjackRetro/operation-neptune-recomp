# Notes

## 2026-10-25 first pass

- `ON.EXE`: MZ, header 13,824 bytes, 3,444 relocations, CS:IP 0000:0000,
  SS:SP 2E0D:00E6. File size equals image size, so no overlay appended.
- `Turbo-C - Copyright (c) 1988 Borland Intl.` string present; startup code is the
  standard Borland prologue (`mov dx,2343h` / `int 21h` AH=30h).
- Resident code is at file offset 0x3600, 160,624 bytes.
- `analyze.py`: 637 functions, 66,265 instructions, 1,057 strings. Its report
  banner says "Sid Meier's Civilization (1991)"; that label is hardcoded in the tool.
- Largest "function" `res_026A10` (16 KB) is NOT code: it is the DGROUP data
  segment (startup loads DGROUP = 0x2343, image 0x23430) decoded as code.
  `largemodel16.detect_code_end` clips it: code ends at image 0x23410.
- Strings reference `A:\TLC\ON\NEP000.DAT`..`NEP003.DAT`, `EGA.DRV`, and a
  "Sound Effects" option.

## Open questions

- `NEP003.DAT` is named in the exe but is not in this folder (only 000-002).
- Large-model far calls: run `largemodel16` (`detect_code_end`, `build_call_graph`)
  to clip the data segment off the code and resolve far calls before lifting.
- Graphics: strings suggest an EGA driver (`EGA.DRV`); confirm video modes used.

## Large-model pass (done)

- 636 real functions after clipping the data blob.
- Call graph: near 910/914 and far 3,028/3,036 resolved (99.7%); 189 roots.

## First lift (`tools/lift_full.py`, adapted from dinopark)

- 639 functions lifted (636 + Borland startup `fn_00000` + 2 startup call
  targets), 0 lift errors, ~71K lines of C into `gen/` (git-ignored).
- 726 near call/jmp targets needed wrap correction.
- Forced function starts: unresolved far and near call targets that land in the
  code region become functions (analyzer misses prologue-less assembly helpers).
  One round adds 97. Result: **736 functions lifted, 0 lift errors**, 6
  unresolved targets, all outside the code region (data decoded as code,
  ignored).
- Caveat: forced starts are not validated against a prologue/instruction
  boundary check (dinopark's `valid_boundary`), so some may be mid-function
  targets. They overlap existing functions, which the lifter tolerates; audit the
  list once the boot runs. Overlap also means switch counts below include
  duplicates.
- Far-call targets show 123 distinct segment values; the real ones are few
  (0, 145, 586, 605, 641, 793, ...). The rest (e.g. 34840, 65535) are noise.

## Jump tables (`jump_tables` / `decode_func` in tools/lift_full.py)

ON.EXE has ~67 distinct `jmp word cs:[bx+table]` Borland switches (plus 7
`jmp cs:[bp+..]`, 5 `call cs:[bx+..]`, not handled).

- Table address is CS*16 + disp, so a wrong segment-map guess reads junk. Where
  a switch fails under the guessed CS, candidate bases (paragraphs up to 64K
  below the function) are tried. A base counts if its table has the right
  length (the `cmp` bound, else a run of >= 3 arms) with every arm inside the
  function, and the table sits right after the `jmp` (<= 8 bytes), or, as a
  fallback for dense switches with an exact `cmp` bound, anywhere in the
  function (tables parked at the end of the routine). One function has one CS,
  so candidates are intersected across all its failing switches; a unique
  survivor overrides the segment map. 7 functions overridden. Applied before
  lifting so wrap fixing, `SEG_xxxx` and `push cs` see it.
- Bound semantics: `cmp bx,N` counts cases only if a `shl bx,1` follows it; with
  no shl, bx is already a byte offset and the table has N/2+1 words
  (fn_01D8C/fn_021E4 tile blitters).
- Sparse shape (`mov cx,N` / `mov bx,list` / loop / `jmp cs:[bx+2N]`): the value
  list (here ASCII command letters in fn_04378) sits right after the jmp and
  the arms follow it. These need the placement rule above: a list of small
  values plus almost any base lands inside the function.
- Tables sit in the code stream, so the linear decode desyncs through them.
  Each function is decoded, tables located, then re-decoded skipping the table
  bytes. **The decoder's `address` and relative-branch `disp` are relative to the
  buffer it is given, not to `base_offset`, so each re-decoded piece is
  re-expressed against the function start.** (A first version of this pass did
  not; it left jumps in every piece after the first pointing at the wrong
  place and inflated the forced-start count from 98 to 138. Fixed.)
- Unbounded dense switches (no `cmp`, e.g. `mov bh,0 / shl bx,1 /
  jmp cs:[bx+0x2007]` in fn_1FDCB, a 5-switch interpreter-style routine): the
  table length is a guess, and the tables are not right after the jmp. All four
  of its tables are valid together at exactly one base (0x1DE90, CS=0x1DE9), so
  intersecting candidates across the function's switches finds it. Then:
  - Nothing can point into a table, so a table ends at the first arm target
    that lies after its start. The 0x246d table mixes internal arms with tail
    jumps to other functions (0x1F052, 0x1DEAE, ...) and ends exactly at its
    first internal arm (0x2033D): 32 entries. This is accepted only when a run
    ends that way (<= 128 entries); otherwise the strict in-function run stands.
  - Tables are also cut where the next table starts.
  - After the table-skipping re-decode, arms outside the function stay (the
    lifter sends them to `recomp_dispatch`), internal arms off an instruction
    boundary are dropped.
  - A forced start nested inside a function whose CS was inferred inherits it.
- Result: **68 of 72** switch sites resolved (964 arms; duplicates from
  overlapping functions included), 736 functions, 0 lift errors. Remaining
  (`NEP_DEBUG_SWITCH=1` lists them, `NEP_DEBUG_INFER=1` shows candidate bases):
  1. Video-mode thunks (fn_008EC / fn_00946 / fn_00AB1: `mov bl, ds:[0x588F]` /
     `shl bx,1` / `jmp cs:[bx+0x3C]`). These are not switches inside a function:
     module 0x910 (CS=0x91) is a video driver and each entry point tail-jumps
     through a per-mode table at 0x94C to the mode-specific routine
     (words 0, 0x1A1, 0, 0x258, 0x294, 0, 0x2A7, 0 -> 0xAB1, 0xB68, 0xBA4,
     0xBB7; zero = unsupported mode). The arms are other functions, so the
     lifter's generic `recomp_dispatch(cpu, cs, target)` is the right output,
     but it needs the right CS (the segment map gives fn_008EC and fn_00AB1 CS=0)
     and the targets (0xB68, 0xBA4 are entries inside fn_00AB1's analyzer
     range) to exist as functions. Left to the dispatch-miss loop.
  2. fn_021E4: a copy of fn_01D8C's tile blitter with a single switch, so two
     adjacent bases tie. Needs a tiebreaker (e.g. the matching table in the
     other copy).

## Not done yet

- The video thunks and fn_021E4 above; `jmp cs:[bp+..]` and `call cs:[bx+..]` forms.
- Borland `_INIT_` table, CRT overrides.
- Validate forced starts; filter noisy segments in the segment map.
- `runtime/recomp16` host (DOS/BIOS/EGA on SDL2) and the CMake build.
