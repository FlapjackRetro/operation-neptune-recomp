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


def jump_tables(data, hdr, insns, cs, lo, hi, io=None, infer=False):
    """Arms of each `jmp word cs:[bx+table]` -- a Borland switch.

    The table is a run of near offsets in the code segment; nothing names the
    arms but this one instruction. The preceding `cmp bx, N` / `jbe` bounds it,
    failing that take entries while they land on an instruction boundary inside
    this function. Two shapes (dinopark found both):

      dense:   cmp bx, N / jbe / jmp default / shl bx, 1 / jmp cs:[bx+table]
      sparse:  mov cx, N / mov bx, values / ... / jmp cs:[bx + N*2]
               (bx walks a list of case VALUES; arm offsets follow the values)

    The table sits at CS*16 + disp, so a wrong CS reads junk. With infer=True,
    a switch that fails under the supplied CS is retried against every
    paragraph-aligned base up to 64K below the function; if exactly one base
    gives a table whose arms all land on instruction boundaries (and whose
    length matches the `cmp` bound), that base is taken as the true CS.

    Arms are accepted when they land inside the function ([lo, hi)), not on a
    decoded instruction boundary: the table lives in the code stream, so the
    linear decode runs through it and desyncs. The caller re-decodes around the
    returned table extents and verifies the arms afterwards.

    Returns ({jmp instruction address: [absolute arm addresses]}, [inferred
    segment values], [(table start, table end) image offsets]).
    """
    out, inferred, tabs, pending = {}, [], [], []

    def arms_at(base, ins, o, bx_imm, cx_imm, bound):
        if bx_imm is not None and cx_imm and o.disp == cx_imm * 2:
            bnd, start = cx_imm, base + bx_imm + o.disp
        else:
            bnd, start = bound, base + o.disp
        arms = []
        for k in range(bnd if bnd else 256):
            a = start + k * 2 + hdr
            if a < 0 or a + 2 > len(data):
                break
            t = base + int.from_bytes(data[a:a + 2], 'little')
            if not (lo <= t < hi):
                break
            arms.append(t)
        if arms and (bnd is None or len(arms) == bnd) and (bnd or len(arms) >= 3):
            first = base + bx_imm if (bx_imm is not None and cx_imm
                                      and o.disp == cx_imm * 2) else start
            return arms, (first, start + 2 * len(arms))
        return None

    for idx, ins in enumerate(insns):
        o = ins.op1
        if ins.mnemonic != 'jmp' or not o or o.type != OpType.MEM:
            continue
        if o.base != 'bx' or o.seg != 'cs':
            continue
        bound = bx_imm = cx_imm = None
        window = insns[max(0, idx - 16):idx]
        for wi, j in enumerate(window):
            if not (j.op1 and j.op1.type == OpType.REG16 and j.op2
                    and j.op2.type in (OpType.IMM8, OpType.IMM16)):
                continue
            reg = repr(j.op1)
            if j.mnemonic == 'cmp':
                # `cmp bx,N` before a `shl bx,1` counts cases; with no shl
                # between it and the jmp, bx is already a byte offset into the
                # word table, so N bytes is N/2 entries (+1: the compiler's
                # jge/ja leaves one spare).
                doubled = any(k.mnemonic == 'shl' and k.op1 and repr(k.op1) in ('bx', 'ax')
                              and k.op2 and k.op2.type == OpType.IMM8 and k.op2.disp == 1
                              for k in window[wi + 1:])
                bound = j.op2.disp + 1 if doubled else j.op2.disp // 2 + 1
            elif j.mnemonic == 'mov' and reg == 'bx':
                bx_imm = j.op2.disp
            elif j.mnemonic == 'mov' and reg == 'cx':
                cx_imm = j.op2.disp
        got = arms_at(cs * 16, ins, o, bx_imm, cx_imm, bound)
        if got:
            out[ins.address] = got[0]
            tabs.append(got[1])
            continue
        if not infer or io is None:
            continue
        strict, loose = {}, {}
        lo_b = max(0, (io - 0xFFF0) & ~0xF)
        for base in range(lo_b, io + 1, 16):
            got = arms_at(base, ins, o, bx_imm, cx_imm, bound)
            if not got:
                continue
            # Borland lays the table (or the sparse value list) right after the
            # jmp. Without this the wrong bases pass: a list of small values
            # plus almost any base lands inside the function.
            if 0 <= got[1][0] - (ins.offset + ins.length) <= 8:
                strict[base] = got
            # Fallback for a table parked at the end of the function: allow it
            # anywhere in the function, but only for a dense switch whose `cmp`
            # fixed the length exactly.
            elif bound and cx_imm is None and lo <= got[1][0] < hi:
                loose[base] = got
        found = strict if strict else loose
        if os.environ.get('NEP_DEBUG_INFER'):
            print(f'  INFER fn@{io:X} jmp@{ins.address:X}: bound={bound} cx={cx_imm} strict={[hex(b) for b in strict]} loose={[hex(b) for b in loose]}')
        pending.append((ins, found))
    # One function has one CS: intersect the candidate bases of every switch
    # that failed under the guessed CS. (Adjacent tables or repeated pairs make
    # a neighbouring base look valid for one switch, never for all of them.)
    sets = [set(f) for _, f in pending if f]
    if sets:
        common = set.intersection(*sets)
        if len(common) == 1:
            base, = common
            for ins_, f in pending:
                if base in f:
                    out[ins_.address] = f[base][0]
                    tabs.append(f[base][1])
            inferred.append(base >> 4)
    return out, inferred, tabs


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

    def decode_func(io, end, cs, infer=False):
        """Decode a function around its switch tables.

        Linear decode first to find the tables, then decode again skipping the
        table bytes so the arms land on real instruction boundaries. A switch
        whose arms still miss a boundary is dropped (it dispatches at runtime).
        """
        insns = Decoder(data[io + HDR:end + HDR], base_offset=io).decode_all()
        jt, inf, tabs = jump_tables(data, HDR, insns, cs, io, end, io=io, infer=infer)
        if tabs:
            cuts, pos = [], io
            for ts, te in sorted(tabs):
                ts, te = max(ts, pos), min(te, end)
                if ts >= te:
                    continue
                cuts.append((pos, ts))
                pos = te
            cuts.append((pos, end))
            insns = []
            for a_, b_ in cuts:
                if b_ > a_:
                    piece = Decoder(data[a_ + HDR:b_ + HDR], base_offset=a_).decode_all()
                    # The decoder's `address` and relative-branch `disp` are
                    # relative to the buffer it was given, not to base_offset.
                    # Re-express both against the function start so every piece
                    # agrees with the first one.
                    shift = a_ - io
                    for x in piece:
                        x.address = x.offset - io
                        o = x.op1
                        if shift and o and o.type in (OpType.REL8, OpType.REL16):
                            o.disp += shift
                    insns += piece
            offs = {x.offset for x in insns}
            jt = {k: v for k, v in jt.items() if all(t in offs for t in v)}
        return insns, jt, inf

    # Segment inference from switch tables: where the segment map guessed wrong
    # the table reads junk; a base that makes every arm land on an instruction
    # boundary is the real CS. Applied before lifting so wrap fixing, SEG_xxxx
    # and `push cs` all see it.
    cs_over, cs_conflict = {}, 0
    for name, io, end in funcs:
        try:
            _, _, inf = decode_func(io, end, cs_of(io), infer=True)
        except Exception:
            continue
        if inf:
            if len(set(inf)) > 1:
                cs_conflict += 1
                continue
            cs_over[io] = inf[0]
    if cs_over:
        print(f"  segment overrides from switch tables: {len(cs_over)} functions "
              f"({cs_conflict} ambiguous, skipped)")
    _cs_of = cs_of
    cs_of = lambda fn: cs_over.get(fn, _cs_of(fn))

    def lift_pass(funcs, known):
        os.makedirs(OUT, exist_ok=True)
        res = dict(bodies=[], calls=set(), lifted=0, failed=[], wrapped=0, segs=set(),
                   switches=0, arms=0, switch_total=0)
        for name, io, end in funcs:
            cs = cs_of(io)
            res["segs"].add(cs)
            try:
                insns, jt, _ = decode_func(io, end, cs, infer=True)
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
                lifter.jump_tables = jt
                res["switches"] += len(jt)
                res["arms"] += sum(len(v) for v in jt.values())
                if os.environ.get("NEP_DEBUG_SWITCH"):
                    for k, x in enumerate(insns):
                        o = x.op1
                        if (x.mnemonic == 'jmp' and o and o.type == OpType.MEM
                                and o.base == 'bx' and o.seg == 'cs'
                                and x.address not in jt):
                            t = cs * 16 + o.disp + HDR
                            words = [int.from_bytes(data[t + q * 2:t + q * 2 + 2], 'little')
                                     for q in range(8)]
                            print(f"  UNRESOLVED {name} cs={cs:04X} jmp@{x.address:X} "
                                  f"disp={o.disp:#x} fn=[{io:X},{end:X}) "
                                  f"prev={[ (q.mnemonic, str(q.op1), str(q.op2)) for q in insns[max(0,k-4):k]]} "
                                  f"words={[hex(w) for w in words]}")
                res["switch_total"] += sum(
                    1 for x in insns if x.mnemonic == 'jmp' and x.op1
                    and x.op1.type == OpType.MEM and x.op1.base == 'bx'
                    and x.op1.seg == 'cs')
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
    print(f"  switches: {res['switches']} resolved of {res['switch_total']} "
          f"`jmp cs:[bx+..]` ({res['arms']} arms)")
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
