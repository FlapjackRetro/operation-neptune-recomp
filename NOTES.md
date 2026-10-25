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

## CRT overrides / floating point

ON.EXE's floating point is Borland 8087-emulator code: every x87 op is
`CD 34..3B <modrm>` (ESC D8..DF), `CD 3C` (segment-override ESC), `CD 3D`
(FWAIT) -- 426 sites in 40 functions. Lifted naively, the bytes after each
`CD 3x` decode as unrelated instructions (that is what made fn_005C7 etc. look
like junk). `lift_full.py` now sets `decode16.EMU87_INTS = True`, which
normalises them to real ESC opcodes, as the emulator does when an 8087 exists.

- `OVERRIDES` table added (same idea as dinopark): listed functions are not
  lifted; the runtime supplies them under the same `fn_XXXXX` name. Listed in
  `gen/recomp_all.h`. First entry: fn_002BE, the emulator installer
  (hooks INT 34h..3Dh through INT 21h/25h). The rest of the emulator
  (0x405..0x5BA) is only called from it, so it goes dead.
- fn_005C7 is `__ftol`-style (fnstcw / or 0xC00 / fldcw / fistp); the game
  calls it from 4 places. Kept lifted.
- No int86/int86x stack-built `int nn` stub exists in this binary (searched
  for `mov [bp+x], 0xCD`), no `sprintf` override chosen yet: those were found
  by dinopark at runtime, and there is no runtime here to fail yet.

**Gap:** pcrecomp's `lift16` emits x87 instructions only as comments
(`/* FPU: esc_N ... */`), so float code is a no-op in the generated C until
the lifter emits real FPU ops (`disasm/fpu_decode.py` and `ne_lift.py` are
the starting points). The runtime also needs an x87 stack (`dos32`/`native32`
have one to borrow).

## x87 lifting (done)

- `lifter.x87 = True` turns on lift16's real x87 path (it was already there,
  just opt-in). `X87Lifter` in `lift_full.py` adds the two opcodes ON.EXE uses
  that lift16 lacks: `fxam` (D9 E5, 9 sites) and `fyl2xp1` (D9 F9).
- The `x87_*` helpers the generated code calls are not in pcrecomp's runtime;
  `src/recomp/x87.h` implements them (8 doubles, TOP, SW, CW; 32/64/80-bit and
  16/32/64-bit-integer loads and stores; integer stores honour the control
  word's rounding field, which Borland's `__ftol` sets to truncate).
  `test/test_x87.c` checks them on the host (`gcc test/test_x87.c -lm`).
  80-bit values are held as double: not bit-exact with a real 8087.
- 8 sites still lower to `x87_unhandled` (d922, dafa, dd2e8a87, ddf6,
  df76f6 in fn_008EC/00946/1F1CC/1F3B4/1FDCB/201D6/2119C). They are not valid
  8087 encodings, so they are almost certainly data lifted as code; the helper
  logs the first 16 and carries on.
- Emulator hooks overridden (all do a bare far return): fn_00283/fn_00343 (the
  `ds:[3F1A]`/`[3F1C]` init/exit hooks that save/restore INT 34h..3Dh/75h),
  fn_002BD/fn_002BE (installer). Native versions are in `src/recomp/overrides.c`.
  Not yet verified: the far-return convention against callers (needs the runtime).
- Function count fell 752 -> 742: the emulator's own data/code no longer
  generates junk forced starts.

## Runtime (first boot)

`cmake -S . -B build && cmake --build build` builds `build/nep` from the lifted
`gen/` plus `src/recomp/` (cpu.h/cpu.c from pcrecomp's recomp16, our
`runtime16.c`, `ega.c`, `overrides.c`, `x87.h`). Run from the repo root:
`build/nep <ON.EXE> <game dir>` (the folder holding NEP00n.DAT; the exe's
`A:\TLC\ON\` paths are mapped to it case-insensitively).

What exists: image loader + PSP/environment, the DOS calls the game makes
(file I/O, memory blocks, vectors, time), BIOS INT 10h/16h/1Ah/11h/15h, port
stubs (PIT, speaker, PIC), a timer interrupt (guest INT 08h run from
`RECOMP_TICK`, 18.2 Hz), and a planar EGA model behind A000:0000 (write modes
0-3, read modes 0/1, latches, bit mask, palette) through `RECOMP_MEM_HOOK`.

Environment: `NEP_TRACE=1` (log DOS/BIOS calls and dispatch misses with a C
backtrace), `NEP_SECONDS=n` (watchdog, default 20, prints the guest call
stack), `NEP_KEYS=...` (scripted keystrokes), `NEP_FRAME=out.ppm` (dump the
EGA screen at exit), `NEP_PLANES=out.bin` (raw planes).

**Dispatch-miss feedback is live.** The runtime writes the in-code addresses it
failed to dispatch to `work/nep_misses.txt`; `lift_full.py` reads that file and
turns them into function starts (`tools/converge.sh` loops lift/build/run until
it stops growing). It found the timer ISR (0x2508), 0x10B8 and a few video
driver entry points. Offsets >= 0xFF00 are ignored as negative-wrapped garbage.
Also fixed: conditional jumps that leave a function are now forced starts
(the argument parser, fn_005FA, runs into 0x6B5).

Status: the game runs the Turbo C startup, probes the video/sound hardware,
sets EGA mode 0Eh, loads resources from NEP000.DAT, starts the timer, and
reaches the main loop (fn_07FC4) and then later screens (fn_0B3A7), where it
blocks on INT 16h. 0 crashes, 0 unhandled DOS calls.

Open: the only picture drawn so far is a band of coloured stripes in the top 50
rows (4096 bytes written with three bit-mask values), which looks like a
hardware test pattern; no real screen has been drawn yet, so it is not yet
clear whether the game draws through a path the hooks miss. Next: find what
fn_0B3A7's screen draws, MPU-401/AdLib probing (port 331/388 loops time out
cleanly), SDL2 window + real keyboard/mouse, EGA.DRV is named by the exe but
absent.

## VGA path (2026-10-25): the palette and the "correct game start"

- On VGA hardware (INT 10h AX=1A00 returns AL=1A, BL=7/8) `fn_02812` sets `ds:[588F]=5`.
  `fn_029A9` then loads the game's 256-entry DAC table (DGROUP+0x13C, INT 10h AX=1012)
  and `fn_02904` calls `fn_03926(0x91, 0, 1)`, which **overwrites the whole video driver
  segment** (image 0x910..0x1070, EGA planar routines) with mode-13h (320x200x256)
  routines, including the jump vectors (`EB 3A 90` / `E9 rel16`).
- The patched bytes cannot be lifted from the file. `NEP_DUMP_VGADRV=work/vga_drv.bin ./build/nep ...`
  (NEP_VGA=1) dumps them from a run; `NEP_VARIANT=vga python3 tools/lift_full.py` lifts them
  as `fv_*` into `gen/vga_*.c` and `recomp_dispatch` uses them whenever the live stub at
  image 0x910 is no longer `E9`. Dispatch misses in that range go to `work/nep_misses_vga.txt`.
  `tools/converge.sh` with `NEP_VGA=1` does all of it.
- Frame check: with NEP_VGA=1 the first frame has background (16,16,16), land (4,65,0), grid (65,32,0),
  identical to the DOSBox-X reference. Without NEP_VGA the EGA planar path renders the same scene
  in EGA colours.
- Bad forced starts that broke things: 00E03 (mid-instruction; E02 is the `es:` prefix), 00A40/003C0
  (`jmp cs:[bx+..]` seen with ds=FFFF), C7CB0 (timer ISR chained through a corrupted vector).
  They appear in the miss file when the game runs with the wrong video driver; remove them.

## SDL2 front end

- `src/recomp/frontend.h` is the seam: `frontend_sdl.c` (window, 4:3 letterbox, nearest scaling, keyboard
  -> BIOS key queue with US scan codes) or `frontend_stub.c` (headless). CMake picks SDL2 when
  `find_package(SDL2)` succeeds (`-DNEP_SDL=OFF` forces headless).
- SDL runs on the main thread; the guest runs on its own 512 MB-stack thread. The guest pushes a rendered
  RGB frame every 16 ms from `recomp_tick`; `INT 16h` waits spin `recomp_tick` so the timer ISR keeps going.
  Closing the window longjmps the guest out through `recomp_tick`.
- Not yet built against real SDL2 (none in the dev sandbox): the file was compile-checked against a
  hand-written header only. Windows/MinGW also needs guards for `execinfo.h` (backtraces), SIGALRM/SIGUSR1
  (watchdog) and `usleep`.

## Timer (PIT) pacing

- The game programs PIT channel 0 (`out 43h,36h` then divisor 0x4000 via `out 40h`) so its INT 08h ISR runs at
  72.8 Hz, not the BIOS 18.2 Hz. `recomp_tick` fires the ISR at the programmed rate (`port_out8` tracks the
  divisor), catching up a few missed ticks at a time. Ignoring the divisor made the intro run 4x slow.
- The window title shows `timer N/s ticks N/s fps` (expect timer ~72/s).
- Windows: `unsigned long` is 32-bit; the host clock is `uint64_t`. GCC 16 rejects implicit declarations, so
  the unresolved-call stubs are declared in `recomp_all.h`.

## Audio

- `src/recomp/audio.cpp` (C API in `audio.h`): AdLib = ymfm YM3812 (vendored in `third_party/ymfm`, BSD-3) on
  ports 388/389, PC speaker = PIT channel 2 (ports 42/43) gated by port 61 bits 0-1 as a square wave.
  The SDL audio callback pulls `audio_render()`; `NEP_WAV=file` writes a headless WAV (44.1 kHz) for checks;
  `NEP_AUDIOLOG=1` logs OPL register writes.
- AdLib detection reads the status port ~100x to wait out the 80 us timer. Recompiled code does that far
  faster than ISA I/O, so each status read advances a 3 us virtual clock (otherwise detection fails and
  there is no music).
- The game also probes port 0x331 (MPU-401); no MIDI output is implemented.

## Backward-branch wrap bug (capsule-exit animation)

The decoder masks relative branch targets to 16 bits (`WRAP_NEAR_TARGETS`).
For a function split out of a larger routine, a *backward* `loop`/`jcc`/`jcxz`/short `jmp`
that leaves the function therefore arrived as e.g. `0xFFD7` and the lifter turned it
into `func_start + 0xFFD7` -- 0x10000 too high.  `tools/lift_full.py` already
wrap-corrected `call`/`jmp rel16`, but not the other branch kinds.  In the object
loop at 09D4:0118 (`loop 0x9E58`) that became a tail call into `fn_19E58` (the C
runtime's number formatter), which trashed the stack (SP fell from ~0xFC0 to 0x65)
and corrupted the Expert scene right after the sub left the capsule.  All relative
branch kinds are wrap-corrected now (970 targets vs 6xx before).

Also fixed: the timer ISR's `pushf; call far <BIOS iret stub F000:FF53>` chain leaked
the flags word on every tick (the iret stub pops ip, cs *and* flags), and
`INT 21h/42h` treated the 32-bit offset as unsigned (SEEK_CUR with negative offsets).

Debug aids (env, off by default): `NEP_SPLOG` (ss:sp + tick counter every 500 ms),
`NEP_SPBT=<sp>` (backtrace first time SP drops below value), `NEP_SPWATCH=<sp>`
(function-entry ring when SP drops below value), `NEP_WATCH_MS` (gdb hook).

## Jump-table handlers must be forced function starts (wrong-puzzle bug)
Handlers reached via `jmp [bx]` tables (object loop table at DS:0x1453, seg 09D4) that sit
inside another lifted function's body are not dispatchable unless they are function starts.
A missed dispatch pops only 4 bytes and leaks stack every frame, which corrupted `fn_08CB4`'s
frame and fed garbage to `fn_0C1EF`, loading puzzle id 0 (Combination Lock) at the wrong
place. Fix: the dispatch-miss feedback (`work/nep_misses.txt`, `tools/converge.sh`) now
includes 0A046 0A055 0A088 0A10A 0A1B8 0A1BF 00815. SP stays constant after the fix.

## Three float bugs found via the Speed Indicator puzzle ("0%", any answer accepted)
1. **FBSTP/FBLD unhandled.** Borland's float-to-text (`fn_2119C`) stores packed BCD (`DF /6`);
   lift16 emitted `x87_unhandled`, so printed numbers came out as 0. `X87Lifter` in
   `tools/lift_full.py` now lifts them to `x87_bcd_wr/rd` (x87.h).
2. **INT 3Ch segment-override table had DS and ES swapped.** The top two bits of the byte after
   `CD 3C` are CS=0, SS=1, DS=2, ES=3 (evidence: `les bx,[bp+6]; CD 3C DD 5F 16` x39 sites, and
   `mov bx,sp; CD 3C 59 3F` = fnstcw ss:[bx]). Every float store/load through an `les` pointer went
   to DS. `lift_full.py` patches `decode16._EMU87_SEG` (the fix belongs upstream in pcrecomp too).
3. **`g_x87` was `static` in x87.h, so every generated chunk had its own FP stack.** A double
   returned in ST(0) across a chunk boundary arrived as 0. Now one global (`X87_SHARED`, defined
   in runtime16.c, set in CMakeLists.txt).

Also: all 25 entries of the object-handler jump table at DS:0x1453 (image 0x9D40+off) are now
forced function starts (work/nep_misses.txt); scene 1's objects hit handlers the first
play-through never reached, leaking 0xC stack bytes per frame and clobbering BP, so the puzzle
category read as 0 (Combination Lock).

Debug hooks added (env-gated): NEP_WATCHSS=<hex ss offset>, NEP_WATCHBP=<hex bp threshold>,
`[ew-words]` line in NEP_ENTERWATCH.

## PC speaker only
`NEP_AUDIO=speaker` hides the AdLib (status port reads 0xFF, OPL writes dropped) so the game's
detection fails and it falls back to the PC speaker. Verified headless: zero OPL writes, speaker
audio present. Default (unset) is AdLib.
