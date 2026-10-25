# Operation Neptune (DOS) recomp

Static recompilation of the 1991 DOS version of Operation Neptune (`ON.EXE`),
using the [pcrecomp](https://github.com/sp00nznet/pcrecomp) toolbox.

Game files live one folder up (`..\`). They are **not** part of this project and
must never be committed (see `.gitignore`).

## Target

| | |
|---|---|
| Binary | `ON.EXE`, 16-bit DOS MZ, 174,448 bytes, no overlay |
| Toolchain | Turbo C (Borland, 1988 runtime), likely large model |
| Packing | None found (not LZEXE / PKLITE) |
| Pipeline | `decode16` -> `analyze` -> `lift16` -> `runtime/recomp16` |
| Data | `NEP000.DAT`..`NEP002.DAT` (the exe also names `NEP003.DAT`, `EGA.DRV`) |

`INSTALL.EXE` is not part of the recomp.

## Layout

```
recomp/
  tools/analyze.py    wrapper: runs pcrecomp's analyze.py on ..\ON.EXE
  work/symbols.toml   first-pass function catalog (637 functions)
  NOTES.md            findings and open questions
```

## Usage

Clone pcrecomp somewhere and point `PCRECOMP` at it:

```
set PCRECOMP=C:\src\pcrecomp
python tools\analyze.py
```

No third-party Python packages are needed for the 16-bit path.
