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
  code region become functions (analyzer misses prologue-less assembly helpers),
  and so do **near tail-jump targets out of a function** (the lifter emits
  those as `recomp_dispatch` without naming the target, so they have to be
  collected separately; missing them left the video driver's entry vector
  jumping into functions that did not exist) and the per-mode arms of the
  video-mode thunks (below). 3 rounds add 113 in all. Result: **752 functions
  lifted, 0 lift errors**, 6 unresolved targets, all outside the code region
  (data decoded as code, ignored).
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
- Result: **69 of 75** switch sites resolved (973 arms; duplicates from
  overlapping functions included), 752 functions, 0 lift errors. All 4 originally unresolved cases are now handled
  (`NEP_DEBUG_SWITCH=1` still lists them, `NEP_DEBUG_INFER=1` shows candidate bases):
  1. Video-mode thunks: solved, but not as switches. Module 0x910 (CS=0x91) is
     the video driver. It starts with an 18-entry vector of `jmp rel16` stubs
     (0x910, 0x913, ...; far calls go to `0x91:0x3` etc., 195 calls to the
     0x913 stub alone). Each stub tail-jumps to a thunk -- `xor bh,bh / mov bl,
     ds:[0x588F] / shl bx,1 / jmp cs:[bx+D]` -- which picks the per-mode
     routine from a table of 5 words (video mode 0..4; 0 = unsupported):
     0xAA4 (D=0x3C) -> 0xAB1, 0xB68, 0xBA4 (int 10h modes 0x0E, 0x09, 0x13);
     0xBAA (D=0x46) -> 0xBB7, 0x10D5, 0x114B. Fixes:
     - tail-jump targets are now forced function starts (fn_00AA4, fn_00BAA);
     - the segment map's nearest-seed fallback only used seeds that were
       analyzer function starts, so the thunks got CS=0 and read their table
       from the wrong place. Any in-code-region far-call target now seeds it;
     - a thunk (a function starting at the `xor bh,bh`) has its table read
       (length = gap to the next thunk's table, else the typical 5) and every
       non-zero arm becomes a forced start. The jmp itself stays a runtime
       `recomp_dispatch(cpu, 0x91, word)`, which is correct since the arms are
       other functions. The analyzer's overlapping ranges fn_008EC / fn_00946 /
       fn_00AB1 contain the same jmp but are not entered as functions, so the
       "unresolved switch" report still lists them; ignore those.
  2. fn_021E4: solved. It is a second blitter in the same module (CS=0x91) whose
     single `jmp cs:[bx+0x15E4]` reads the *same* table fn_01D8C already uses
     (0x1EF4, arms 0x1E69 / 0x1E51 / 0x1E3E / 0x1E2B ... inside fn_01D8C), so no
     arm is inside fn_021E4 and it could not be resolved on its own (two adjacent
     bases tied). The segment-map seeding fix above already gave it CS=0x91, so
     its jmp lifts to `recomp_dispatch(cpu, 0x91, word)` correctly. What was
     missing was the arms as functions: lift_pass now registers every validated
     table by (CS base, disp); a switch that failed reads the same key and its
     arms become forced starts (9 arm reads, 5 new functions). If its CS was
     guessed wrong but exactly one validated base fits, that CS is adopted.

## Not done yet

- `jmp cs:[bp+..]` (7) and `call cs:[bx+..]` (5) forms.
- (done: no _INIT_ table exists, hook vectors handled) Borland `_INIT_` table, CRT overrides.
- Validate forced starts; filter noisy segments in the segment map.
- `runtime/recomp16` host (DOS/BIOS/EGA on SDL2) and the CMake build.

## Init table / startup hooks

ON.EXE has no TC++-style `_INIT_` table of 6-byte {flag, priority, far ptr}
records (no `mov si,_INIT_ / mov di,_INITEND` pair in the startup). Turbo C 2.0
instead calls three near hooks through DGROUP vectors, each `push cs / call
word ds:[vec]`: `[3F1A]`=0x283, `[3F1C]`=0x343, `[3F1E]`=0x1A6 (a bare `retf`
stub). The code pointers live only in data, so the driver now reads those
vectors from DGROUP and turns the targets into functions (0x1A6 joins the
startup subroutines; 0x283/0x343 sit inside analyzer ranges and are forced
starts). Result: 755 functions, 0 lift errors, 115 forced starts. The
`init_list_targets` step is therefore not needed for this binary.
