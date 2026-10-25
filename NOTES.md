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
  One round added 98 (far + near), converging in one pass: **737 functions
  lifted, 0 lift errors, 9 unresolved call targets**. Those 9 are far/near
  targets outside the code region (`far_2341_0000`, `far_FFFF_9A20`,
  `res_02A7D5`, ...): data decoded as code, ignored for now.
- Caveat: forced starts are not validated against a prologue/instruction
  boundary check (dinopark's `valid_boundary`), so some of the 98 may be
  mid-function targets. They overlap existing functions, which the lifter
  tolerates, but the list should be audited once the boot runs.
- Far-call targets show 123 distinct segment values; the real ones are few
  (0, 145, 586, 605, 641, 793, ...). The rest (e.g. 34840, 65535) are noise.
  The segment map reports 113 distinct code segments, likely inflated by it.

## Not done yet

- Jump tables / switch arms, Borland `_INIT_` table, CRT overrides.
- Validate forced starts; filter noisy segments in the segment map.
- `runtime/recomp16` host (DOS/BIOS/EGA on SDL2) and the CMake build.
