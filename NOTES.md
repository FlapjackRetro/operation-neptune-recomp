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
  After the jump-table work below: **777 functions lifted, 0 lift errors**,
  138 forced starts over 2 rounds, 17 unresolved targets (all outside the code
  region: data decoded as code, ignored).
- Caveat: forced starts are not validated against a prologue/instruction
  boundary check (dinopark's `valid_boundary`), so some may be mid-function
  targets. They overlap existing functions, which the lifter tolerates, but
  audit the list once the boot runs. Overlap also means switch counts below
  include duplicates.
- Far-call targets show 123 distinct segment values; the real ones are few
  (0, 145, 586, 605, 641, 793, ...). The rest (e.g. 34840, 65535) are noise.

## Jump tables (`jump_tables` in tools/lift_full.py)

ON.EXE has ~67 distinct `jmp word cs:[bx+table]` Borland switches (plus 7
`jmp cs:[bp+..]`, 5 `call cs:[bx+..]`, not handled).

- Table address is CS*16 + disp, so a wrong segment-map guess reads junk. Where
  a switch fails under the guessed CS, `jump_tables(infer=True)` tries every
  paragraph base within 64K below the function; if exactly one base gives a
  table of the right length (the `cmp bx,N` bound) it is taken as the real CS
  and overrides the segment map for that function (8 functions on the first
  pass). Applied before lifting, so wrap fixing, `SEG_xxxx` and `push cs` see it.
- Tables sit in the code stream, so the linear decode desyncs through them.
  Arms are range-checked against the function, then the function is re-decoded
  skipping the table bytes, and arms that still miss an instruction boundary
  are dropped.
- Result: **50 of 76** switch sites resolved (731 arms), including duplicates
  from overlapping functions. 26 remain unresolved and fall back to runtime
  dispatch (`NEP_DEBUG_SWITCH=1` prints each with its CS, bound and table words).
  They fall into two groups:
  1. Video-mode dispatch (`mov bl, ds:[0x588F]` / `shl bx,1` / `jmp cs:[bx+0x3C]`
     in the 0x8EC-0xBB7 routine). With CS=0x91 the table words look right
     (0x1A1, 0x258, 0x294, 0x2A7 -> 0xAB1, 0xB68, 0xBA4, 0xBB7) but arms leave the
     analyzer's function bounds and some entries are 0. This is one routine the
     analyzer split at `retf`s; needs the real function extent.
  2. The sparse shape (`inc bx / inc bx / loop / jmp default` then
     `jmp cs:[bx+N]`): in fn_04378, fn_10827, fn_1D5EF and others the `mov cx`/
     `mov bx` setup is not matching, so the table lands on junk. Needs a look at
     the actual setup sequence.

## Not done yet

- The 26 unresolved switches above; `jmp cs:[bp+..]` and `call cs:[bx+..]` forms.
- Borland `_INIT_` table, CRT overrides.
- Validate forced starts; filter noisy segments in the segment map.
- `runtime/recomp16` host (DOS/BIOS/EGA on SDL2) and the CMake build.
