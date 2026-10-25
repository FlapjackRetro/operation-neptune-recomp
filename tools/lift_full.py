#!/usr/bin/env python3
"""
lift_full.py - first-cut lift of ON.EXE (Operation Neptune, DOS) to C.

Adapted from dinopark/tools/lift_full.py (same Borland/Turbo C large model).
Works in image-offset space (file offset - header size), so a seg:off far
target maps to seg*16+off.

What this does:
  * decodes every function the analyzer found in the code region (data blob
    clipped by largemodel16.detect_code_end)
  * lifts the Borland c0 startup (image 0 .. first function) as fn_00000, and
    its near-call targets as their own functions
  * recovers each function's code segment (needed for near-call wrap fixing)
  * writes chunked C into gen/ (git-ignored) and prints a lift report

NOT done yet (dinopark needed all of these; expect to add them as the boot
bring-up hits them):
  * jump tables / switch arms (cs:[bx+table])
  * Borland _INIT_ table entries (init_list_targets)
  * native overrides of CRT routines, compiled-blit patching
  * runtime/recomp16 host (DOS/BIOS/EGA) and the CMake build
  * dispatch-miss feedback loop

Usage:  set PCRECOMP_HOME=C:\\src\\pcrecomp
        python tools\\lift_full.py [path\\to\\ON.EXE]
"""
import bisect
import collections
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def _pcrecomp_home():
    env = os.environ.get("PCRECOMP_HOME") or os.environ.get("PCRECOMP")
    for c in ([env] if env else []) + [os.path.join(ROOT, "..", "pcrecomp")]:
        if c and os.path.isdir(os.path.join(c, "tools", "disasm")):
            return os.path.abspath(c)
    raise SystemExit("Set PCRECOMP_HOME to your checkout of "
                     "https://github.com/sp00nznet/pcrecomp")


TOOLS = _pcrecomp_home()
sys.path.insert(0, os.path.join(TOOLS, "tools", "disasm"))
sys.path.insert(0, os.path.join(TOOLS, "tools", "lift"))
import analyze            # noqa: E402
import largemodel16 as lm  # noqa: E402
import lift16             # noqa: E402
from decode16 import Decoder, OpType  # noqa: E402
from lift16 import Lifter  # noqa: E402

DEFAULT_EXE = os.path.join(os.path.dirname(ROOT), "ON.EXE")
OUT = os.path.join(ROOT, "gen")
CHUNK = 60
ENTRY_IMG = 0x0000                      # MZ CS:IP = 0000:0000


def build_segmap(ends, decode):
    """image offset -> the code segment (CS) each function runs under.

    1. A far call/jmp states its target's CS outright (seed).
    2. A near call cannot leave its segment: flood-fill across near edges.
    3. Otherwise take the nearest seeded function below that still fits.
    """
    seg, near = {}, collections.defaultdict(set)
    for s in sorted(ends):
        for i in decode(s):
            o = i.op1
            if i.mnemonic in ("call", "call far", "jmp", "jmp far") and o \
                    and o.type == OpType.FAR:
                seg[(o.far_seg * 16 + o.disp) & 0xFFFFF] = o.far_seg
            elif i.mnemonic == "call" and o and o.type == OpType.REL16:
                t = s + o.disp
                if t in ends:
                    near[s].add(t)
                    near[t].add(s)
    seg[ENTRY_IMG] = 0
    fits = lambda fn, cs: 0 <= fn - cs * 16 < 0x10000
    work = [s for s in seg if s in ends]
    while work:
        a = work.pop()
        for b in near[a]:
            if b not in seg and fits(b, seg[a]):
                seg[b] = seg[a]
                work.append(b)
    ks = sorted(k for k in seg if k in ends or k == ENTRY_IMG)

    def cs_of(fn):
        if fn in seg and fits(fn, seg[fn]):
            return seg[fn]
        i = bisect.bisect_right(ks, fn) - 1
        while i >= 0:
            if fits(fn, seg[ks[i]]):
                return seg[ks[i]]
            i -= 1
        return fn >> 4
    print(f"  segments: {sum(1 for s in ends if s in seg)} far-seeded, "
          f"{len(set(cs_of(s) for s in ends))} distinct code segments")
    return cs_of


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EXE
    data = open(exe, "rb").read()

    a = analyze.Analyzer(data)
    a.find_overlays()
    a.detect_all_functions()
    HDR = a.hdr_size
    fs, code_end = lm.real_functions(a)
    print(f"{os.path.basename(exe)}: hdr 0x{HDR:X}, code ends at image "
          f"0x{code_end - HDR:X}, {len(fs)} functions")

    det_end = {f.start - HDR: f.end - HDR for f in fs}
    starts = set(det_end)
    first_det = min(starts)

    # Borland c0 startup: image 0 .. first detected function.
    call_tgts = set()
    try:
        for ins in Decoder(data[HDR:first_det + HDR], base_offset=0).decode_all():
            if ins.mnemonic == "call" and ins.op1 and ins.op1.type == OpType.REL16:
                t = ins.op1.disp & 0xFFFF
                if 0 < t < first_det:
                    call_tgts.add(t)
    except Exception:
        pass

    funcs = [("fn_00000", 0, first_det)]
    ct = sorted(call_tgts)
    for i, s in enumerate(ct):
        funcs.append((f"fn_{s:05X}", s, ct[i + 1] if i + 1 < len(ct) else first_det))
    for s in sorted(starts):
        funcs.append((f"fn_{s:05X}", s, det_end[s]))
    seen = {}
    for name, s, e in funcs:
        if s not in seen or s in det_end:
            seen[s] = (name, s, det_end.get(s, e))
    funcs = sorted(seen.values(), key=lambda t: t[1])
    known = {io: name for name, io, _ in funcs}
    print(f"  startup: fn_00000=[0,0x{first_det:X}) + {len(ct)} call-target subroutines")

    def decode(s):
        try:
            return Decoder(data[s + HDR:det_end.get(s, s) + HDR], base_offset=s).decode_all()
        except Exception:
            return []
    cs_of = build_segmap(det_end, decode)

    code_end_img = code_end - HDR

    def flow_end(t, cap):
        """End of a forced function: follow control flow to the first ret/retf
        that no earlier jump reaches past (next start is the usual answer)."""
        try:
            insns = Decoder(data[t + HDR:cap + HDR], base_offset=t).decode_all()
        except Exception:
            return None
        reach = t
        for ins in insns:
            o = ins.op1
            if (ins.mnemonic.startswith(('j', 'loop')) and o
                    and o.type in (OpType.REL8, OpType.REL16)):
                reach = max(reach, t + (o.disp & 0xFFFF))
            if ins.mnemonic in ('ret', 'ret far', 'retf') and ins.offset >= reach:
                return ins.offset + ins.length
        return None

    def lift_pass(funcs, known):
        os.makedirs(OUT, exist_ok=True)
        res = dict(bodies=[], calls=set(), lifted=0, failed=[], wrapped=0, segs=set())
        for name, io, end in funcs:
            cs = cs_of(io)
            res["segs"].add(cs)
            try:
                insns = Decoder(data[io + HDR:end + HDR], base_offset=io).decode_all()
                base = cs * 16
                for ins in insns:             # wrap-correct near call/jmp targets
                    o = ins.op1
                    if ins.mnemonic in ("call", "jmp") and o and o.type == OpType.REL16:
                        t = base + ((io - base + o.disp) & 0xFFFF)
                        if t != io + o.disp:
                            o.disp = t - io
                            res["wrapped"] += 1
                lifter = Lifter(hdr_size=HDR, known_funcs=known)
                lifter.dispatch = True
                lifter.far_base = 0
                lift16._CODE_SEG = f"{cs:04X}"
                body = lifter.lift_function(name, insns, io, is_far=True)
                body = body.replace("(CPU *cpu)\n{",
                                    f"(CPU *cpu)\n{{\n    cpu->cs = SEG_{cs:04X};", 1)
                res["bodies"].append(body)
                res["calls"] |= lifter.func_calls
                res["lifted"] += 1
            except Exception as e:
                res["failed"].append((name, str(e)))
                res["bodies"].append(f"/* {name}: lift failed: {e} */\n"
                                     f"void {name}(CPU*cpu){{(void)cpu;}}")
        return res

    # Iterate: unresolved far calls whose target is real code become forced
    # function starts (assembly helpers have no prologue, so the analyzer
    # misses them). Far targets past the code region are data noise: ignored.
    forced_total, ignored = 0, set()
    for rnd in range(6):
        res = lift_pass(funcs, known)
        new = set()
        for c in res["calls"]:
            if c in known.values():
                continue
            if c.startswith("far_"):
                _, sg, off = c.split("_")
                t = int(sg, 16) * 16 + int(off, 16)
            elif c.startswith("res_"):        # near target the analyzer missed
                t = int(c[4:], 16)
            else:
                continue
            if 0 <= t < code_end_img:
                if t not in known:
                    new.add(t)
            else:
                ignored.add(c)
        if not new:
            break
        allpts = sorted(set(known) | new)
        for t in sorted(new):
            i = allpts.index(t)
            e = allpts[i + 1] if i + 1 < len(allpts) else None
            e = e or flow_end(t, min(code_end_img, t + 0x400)) or t + 0x40
            funcs.append((f"fn_{t:05X}", t, e))
            known[t] = f"fn_{t:05X}"
        funcs.sort(key=lambda x: x[1])
        forced_total += len(new)
        print(f"  round {rnd + 1}: {len(new)} forced function starts (far + near)")
    bodies, all_calls, lifted = res["bodies"], res["calls"], res["lifted"]
    failed, n_wrapped, segs_used = res["failed"], res["wrapped"], res["segs"]
    print(f"  forced starts total: {forced_total}; ignored out-of-code far targets: {len(ignored)}")

    names = [n for n, _, _ in funcs]
    stubs = sorted(c for c in all_calls if c not in set(names))
    for old in os.listdir(OUT):
        if old.startswith("recomp_") and old.endswith((".c", ".h")):
            os.remove(os.path.join(OUT, old))
    for i in range(0, len(bodies), CHUNK):
        with open(os.path.join(OUT, f"recomp_{i // CHUNK:03d}.c"), "w") as f:
            f.write('#include "recomp_all.h"\n\n' + "\n\n".join(bodies[i:i + CHUNK]) + "\n")
    with open(os.path.join(OUT, "recomp_all.h"), "w") as f:
        f.write("#ifndef RECOMP_ALL_H\n#define RECOMP_ALL_H\n")
        f.write('#include "cpu.h"\n#include "runtime16.h"\n\n')
        for sg in sorted(segs_used):
            f.write(f"#define SEG_{sg:04X} 0x{sg:04X}\n")
        f.write("\n")
        for n in names:
            f.write(f"void {n}(CPU *cpu);\n")
        f.write("\n#endif\n")

    print(f"\nlifted {lifted}/{len(funcs)} functions, {len(failed)} lift errors, "
          f"{n_wrapped} wrapped near targets, {len(stubs)} unresolved call targets")
    if stubs:
        print("  unresolved call targets (first 12): " + ", ".join(stubs[:12]))
    for n, e in failed[:20]:
        print(f"  FAILED {n}: {e}")
    print(f"output: {OUT}  ({(len(bodies) + CHUNK - 1) // CHUNK} chunks)")


if __name__ == "__main__":
    main()
