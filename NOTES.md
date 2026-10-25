# Notes

## 2026-10-25 first pass

- `ON.EXE`: MZ, header 13,824 bytes, 3,444 relocations, CS:IP 0000:0000,
  SS:SP 2E0D:00E6. File size equals image size, so no overlay appended.
- `Turbo-C - Copyright (c) 1988 Borland Intl.` string present; startup code is the
  standard Borland prologue (`mov dx,2343h` / `int 21h` AH=30h).
- Resident code is at file offset 0x3600, 160,624 bytes.
- `analyze.py`: 637 functions, 66,265 instructions, 1,057 strings. Its report
  banner says "Sid Meier's Civilization (1991)"; that label is hardcoded in the tool.
- Largest function: `res_026A10` (16 KB, categorised init) - likely the Borland
  startup / data-segment init area; worth checking first.
- Strings reference `A:\TLC\ON\NEP000.DAT`..`NEP003.DAT`, `EGA.DRV`, and a
  "Sound Effects" option.

## Open questions

- `NEP003.DAT` is named in the exe but is not in this folder (only 000-002).
- Large-model far calls: run `largemodel16` (`detect_code_end`, `build_call_graph`)
  to clip the data segment off the code and resolve far calls before lifting.
- Graphics: strings suggest an EGA driver (`EGA.DRV`); confirm video modes used.

## Next

1. Large-model pass with `largemodel16`.
2. Write `tools/lift_full.py` (start from pcrecomp's dinopark example).
3. Stub DOS/BIOS/EGA on `runtime/recomp16` + SDL2.
