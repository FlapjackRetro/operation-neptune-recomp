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
  * (Borland _INIT_ table: not present in ON.EXE; startup hook vectors handled)
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
import decode16  # noqa: E402

# ON.EXE's floating point is Borland 8087-emulator code (CD 34..3D + modrm).
# Normalise it to real ESC opcodes, as the emulator does when an 8087 exists.
decode16.EMU87_INTS = True

DEFAULT_EXE = os.path.join(os.path.dirname(ROOT), "ON.EXE")
OUT = os.path.join(ROOT, "gen")
CHUNK = 60
# The VGA video driver: type-5 hardware makes fn_03926 overwrite the driver
# segment (image 0x910..0x1070, EGA planar routines) with mode-13h routines.
# The patched bytes are dumped from a run (NEP_DUMP_VGADRV) to work/vga_drv.bin
# and lifted as a variant (fv_*), dispatched when the live stub is not E9.
VARIANT = os.environ.get("NEP_VARIANT") == "vga"
V_LO, V_HI = 0x94C, 0x1070
ENTRY_IMG = 0x0000                      # MZ CS:IP = 0000:0000


def build_segmap(ends, decode, code_end_img):
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
    # Any far-call target inside the code region states its CS, whether or not
    # the analyzer found a function there (a driver's entry vector at 0x910 is
    # a run of `jmp` stubs, not functions). Seeds outside the code region are
    # data decoded as code and are left out.
    ks = sorted(k for k in seg if (k in ends or k == ENTRY_IMG or 0 <= k < code_end_img)
                and fits(k, seg[k]))

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
    failing that take entries while they land inside this function. Shapes
    (dinopark found the first two):

      dense:   cmp bx, N / jbe / jmp default / shl bx, 1 / jmp cs:[bx+table]
      sparse:  mov cx, N / mov bx, values / ... / jmp cs:[bx + N*2]
               (bx walks a list of case VALUES; arm offsets follow the values)
      unbounded dense: no cmp at all (`mov bh,0 / shl bx,1 / jmp cs:[bx+t]`,
               a dispatch on a byte the program keeps in range itself)

    The table sits at CS*16 + disp, so a wrong CS reads junk. With infer=True,
    a switch that fails under the supplied CS is retried against every
    paragraph-aligned base up to 64K below the function. A function has one CS,
    so the candidates of all its failing switches are intersected; one survivor
    is taken as the true CS. Candidates must have the table right after the jmp
    or, for a dense switch, anywhere in the function.

    Arms are accepted when they land inside the function ([lo, hi)), not on a
    decoded instruction boundary: the table lives in the code stream, so the
    linear decode runs through it and desyncs. The caller re-decodes around the
    returned table extents and verifies the arms afterwards. An unbounded table
    is cut where the next table starts (tables are laid out back to back).

    Returns (out, inferred, tmap, unbounded):
      out        {jmp instruction address: [absolute arm addresses]}
      inferred   [segment values taken from the tables]
      tmap       {jmp instruction address: (table start, table end)} image offs
      unbounded  set of jmp addresses whose table length was a guess
      bases      {jmp instruction address: the CS*16 its table was read under}
    """
    out, inferred, tmap, unb, pending, bases = {}, [], {}, set(), [], {}

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
        # bounded: exact length; unbounded: a run of 3+ (4+ when hunting a base)
        if arms and (bnd is None or len(arms) == bnd) and (bnd or len(arms) >= 3):
            first = base + bx_imm if (bx_imm is not None and cx_imm
                                      and o.disp == cx_imm * 2) else start
            return arms, (first, start + 2 * len(arms)), not bnd
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
            tmap[ins.address] = got[1]
            bases[ins.address] = cs * 16
            if got[2]:
                unb.add(ins.address)
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
            # Fallback for a table parked elsewhere in the function: dense
            # switches only (no sparse value list); an unbounded one needs a
            # 4+ run to count.
            elif cx_imm is None and lo <= got[1][0] < hi and (bound or len(got[0]) >= 4):
                loose[base] = got
        found = strict if strict else loose
        if os.environ.get('NEP_DEBUG_INFER'):
            print(f'  INFER fn@{io:X} jmp@{ins.address:X}: bound={bound} cx={cx_imm} '
                  f'strict={[hex(b) for b in strict]} loose={[hex(b) for b in loose]}')
        pending.append((ins, found))
    sets = [set(f) for _, f in pending if f]
    if sets:
        common = set.intersection(*sets)
        if len(common) == 1:
            base, = common
            for ins_, f in pending:
                if base in f:
                    out[ins_.address] = f[base][0]
                    tmap[ins_.address] = f[base][1]
                    bases[ins_.address] = base
                    if f[base][2]:
                        unb.add(ins_.address)
            inferred.append(base >> 4)
    # Unbounded table: some entries may be tail jumps to other functions (outside
    # [lo, hi)), so the strict run above can stop early. Nothing can point into
    # the table itself, so the table ends at the first arm target that lies
    # after its start. If a run of entries ends exactly there (<= 128 entries),
    # that is the real table; otherwise keep the strict run (a longer run would
    # just be junk words that happen to land in range).
    for addr in list(unb):
        base = bases[addr]
        first, _ = tmap[addr]
        arms, limit, k = [], None, 0
        while k < 128:
            pos = first + 2 * k
            if limit is not None and pos >= limit:
                break
            a = pos + hdr
            if a + 2 > len(data):
                limit = None
                break
            t = base + int.from_bytes(data[a:a + 2], 'little')
            if not (base <= t < hi):
                limit = None
                break
            arms.append(t)
            if t >= first + 2:
                limit = t if limit is None else min(limit, t)
            k += 1
        else:
            limit = None
        if limit is not None and len(arms) >= len(out[addr]):
            out[addr] = arms
            tmap[addr] = (first, first + 2 * len(arms))
    # Unbounded tables run into the next table; cut each where the next starts.
    starts = sorted({t[0] for t in tmap.values()})
    for addr in list(unb):
        first, end = tmap[addr]
        nxt = next((x for x in starts if x > first), None)
        if nxt is not None and end > nxt:
            n = (nxt - first) // 2
            out[addr] = out[addr][:n]
            tmap[addr] = (first, first + 2 * n)
            if n < 3:
                del out[addr], tmap[addr]
                unb.discard(addr)
    return out, inferred, tmap, unb, bases


# Functions skipped by the lifter and supplied natively by the runtime
# (same fn_XXXXX name, so callers and the dispatch table are unchanged).
OVERRIDES = {
    0x002BD: 'push ds + fall into the emulator installer fn_002BE (an _fpreset-style'
             ' re-init; called from game code at 2 sites); net effect is a far return',
    0x00283: 'emulator init hook (ds:[3F1A]): saves INT 34h..3Dh/75h, then falls into'
             ' fn_002BD/fn_002BE; no emulator is installed here, so a no-op',
    0x00343: 'emulator exit hook (ds:[3F1C]): restores the saved INT 34h..3Dh/75h'
             ' vectors; nothing was installed, so a no-op',
    0x002BE: 'Borland 8087-emulator installer: hooks INT 34h..3Dh (and 3Eh/3Fh) via'
             ' INT 21h/25h and probes for an FPU. FP sites are normalised to'
             ' real ESC opcodes at lift time, so the emulator never runs; the'
             ' rest of the emulator (0x405..0x5BA) is only reachable from here',
}


class X87Lifter(Lifter):
    """lift16's x87 table plus the opcodes ON.EXE uses that it lacks."""
    def _lift_x87(self, inst):
        if inst.fpu[0] == 0xD9 and len(inst.fpu) == 2:
            if inst.fpu[1] == 0xE5:
                return 'x87_fxam();'
            if inst.fpu[1] == 0xF9:                    # fyl2xp1
                return 'X87_ST(1) = X87_ST(1) * log2(X87_ST(0) + 1.0); x87_pop();'
        return super()._lift_x87(inst)


def main():
    exe = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_EXE
    data = open(exe, "rb").read()
    if VARIANT:
        blob = open(os.path.join(ROOT, "work", "vga_drv.bin"), "rb").read()
        data = bytearray(data)
        data[0x3600 + 0x910:0x3600 + 0x910 + len(blob)] = blob
        data = bytes(data)

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

    # Turbo C 2.0 startup hooks: `push cs / call word ds:[vec]` where the
    # vector (_cleanup / _checknull / _restorezero) is a near pointer into the
    # startup segment, initialised in DGROUP. No TC++-style _INIT_ table exists
    # in ON.EXE; these vectors are its only data-referenced code pointers.
    n_vec = 0
    vec_tgts = set()
    try:
        sb = data[HDR:HDR + first_det]
        k = sb.find(b"\xba")                    # first `mov dx, DGROUP`
        dg = int.from_bytes(sb[k + 1:k + 3], "little") * 16
        for ins in Decoder(sb, base_offset=0).decode_all():
            o = ins.op1
            if (ins.mnemonic == "call" and o and o.type == OpType.MEM
                    and not o.base and not getattr(o, "index", None) and o.size == 2):
                a_ = dg + o.disp + HDR
                t = int.from_bytes(data[a_:a_ + 2], "little")
                if 0 < t < first_det:
                    call_tgts.add(t)
                    n_vec += 1
                elif first_det <= t < code_end - HDR:
                    vec_tgts.add(t)         # inside an analyzer range: force a start
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
    print(f"  startup: fn_00000=[0,0x{first_det:X}) + {len(ct)} call-target subroutines "
          f"({n_vec} via startup hook vectors)")

    def decode(s):
        try:
            return Decoder(data[s + HDR:det_end.get(s, s) + HDR], base_offset=s).decode_all()
        except Exception:
            return []
    cs_of = build_segmap(det_end, decode, code_end - HDR)

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
        table bytes so the arms land on real instruction boundaries. A bounded
        switch with an arm off a boundary is dropped (it dispatches at
        runtime); an unbounded one keeps its leading arms that are on
        boundaries (its length was a guess) and the function is decoded again
        with the shortened table.
        """
        def piecewise(tabs):
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
            return insns

        insns = Decoder(data[io + HDR:end + HDR], base_offset=io).decode_all()
        jt, inf, tmap, unb, bases = jump_tables(data, HDR, insns, cs, io, end, io=io, infer=infer)
        if tmap:
            for _ in range(2):
                insns = piecewise(set(tmap.values()))
                offs = {x.offset for x in insns}
                changed = False
                for addr in list(jt):
                    arms = jt[addr]
                    if addr in unb:
                        # arms outside this function are tail jumps (runtime
                        # dispatch); only internal arms can be gotos, and an
                        # internal arm off an instruction boundary is dropped.
                        keep = [t for t in arms if not (io <= t < end) or t in offs]
                        n = sum(1 for t in keep if io <= t < end)
                        if len(keep) < len(arms):
                            jt[addr] = keep
                            changed = True
                        arms = keep
                        if n < 1:
                            if os.environ.get('NEP_DEBUG_SWITCH'):
                                print(f"  DROP fn@{io:X} jmp@{addr:X} unbounded arms="
                                      f"{[hex(t) for t in arms[:8]]} on-boundary prefix={n}")
                            del jt[addr], tmap[addr]
                            changed = True
                    elif not all(t in offs for t in arms):
                        del jt[addr], tmap[addr]
                        changed = True
                if not changed:
                    break
            else:
                insns = piecewise(set(tmap.values()))
        return insns, jt, inf, bases

    # Segment inference from switch tables: where the segment map guessed wrong
    # the table reads junk; a base that makes every arm land on an instruction
    # boundary is the real CS. Applied before lifting so wrap fixing, SEG_xxxx
    # and `push cs` all see it.
    cs_over, cs_conflict = {}, 0
    for name, io, end in funcs:
        try:
            _, _, inf, _b = decode_func(io, end, cs_of(io), infer=True)
        except Exception:
            continue
        if inf:
            if len(set(inf)) > 1:
                cs_conflict += 1
                continue
            cs_over[io] = inf[0]
    def propagate_cs():
        """A function nested in one whose CS a switch table gave inherits it
        (forced starts overlap the analyzer's function and share its module)."""
        ends_by_start = {io: end for _, io, end in funcs}
        for io, cs in list(cs_over.items()):
            if io not in ends_by_start:
                continue
            for _, io2, _e in funcs:
                if io2 != io and io <= io2 < ends_by_start[io] and io2 not in cs_over \
                        and 0 <= io2 - cs * 16 < 0x10000:
                    cs_over[io2] = cs

    propagate_cs()
    if cs_over:
        print(f"  segment overrides from switch tables: {len(cs_over)} functions "
              f"({cs_conflict} ambiguous, skipped)")
    _cs_of = cs_of
    cs_of = lambda fn: cs_over.get(fn, _cs_of(fn))

    def lift_pass(funcs, known):
        os.makedirs(OUT, exist_ok=True)
        res = dict(bodies=[], calls=set(), lifted=0, failed=[], wrapped=0, segs=set(),
                   switches=0, arms=0, switch_total=0, tail=set(), thunks=set(),
                   tables={}, unres=[], overridden=[])
        for name, io, end in funcs:
            cs = cs_of(io)
            res["segs"].add(cs)
            if io in OVERRIDES:
                res["overridden"].append(name)
                continue
            if VARIANT and not (V_LO <= io < V_HI):
                if 0x910 <= io < V_LO and (io - 0x910) % 3 == 0:    # patched jump vector: seed its target
                    b0 = data[io + HDR]
                    if b0 == 0xE9:
                        res["tail"].add(0x910 + ((io - 0x910 + 3 + int.from_bytes(data[io+HDR+1:io+HDR+3], "little")) & 0xFFFF))
                    elif b0 == 0xEB:
                        r8 = data[io + HDR + 1]; r8 -= 256 if r8 > 127 else 0
                        res["tail"].add(io + 2 + r8)
                continue
            if not VARIANT and 0x910 <= io < 0x94C and (io - 0x910) % 3 == 0 and data[io + HDR] == 0xE9:
                # Video-driver jump vector: `jmp rel16` stubs that the driver's
                # init rewrites at run time to select the EGA/VGA routines.
                # Lifting the static bytes would freeze the initial targets, so
                # decode the jmp from guest memory on every call.
                rel0 = int.from_bytes(data[io + HDR + 1:io + HDR + 3], "little")
                t0 = 0x910 + ((io - 0x910 + 3 + rel0) & 0xFFFF)
                res["tail"].add(t0)
                res["bodies"].append(
                    f"void {name}(CPU *cpu)\n{{\n    cpu->cs = 0x91;\n"
                    f"    uint32_t a = 0x{io:05X}u;\n"
                    f"    uint16_t off = 0; int ok = 1;\n"
                    f"    if (cpu->mem[a] == 0xE9) off = (uint16_t)(a - 0x910 + 3 + (uint16_t)(cpu->mem[a + 1] | (cpu->mem[a + 2] << 8)));\n"
                    f"    else if (cpu->mem[a] == 0xEB) off = (uint16_t)(a - 0x910 + 2 + (int8_t)cpu->mem[a + 1]);\n"
                    f"    else ok = 0;\n"
                    f"    if (ok && !recomp_dispatch(cpu, 0x91, off)) cpu->sp += 4;\n"
                    f"}}")
                res["lifted"] += 1
                continue
            try:
                insns, jt, _, jbases = decode_func(io, end, cs, infer=True)
                base = cs * 16
                for ins in insns:             # wrap-correct near call/jmp targets
                    o = ins.op1
                    if ins.mnemonic in ("call", "jmp") and o and o.type == OpType.REL16:
                        t = base + ((io - base + o.disp) & 0xFFFF)
                        if t != io + o.disp:
                            o.disp = t - io
                            res["wrapped"] += 1
                # Near `jmp` out of the function is a tail call. The lifter emits
                # it as recomp_dispatch without naming the target, so the
                # forced-start search never sees it unless collected here.
                for ins in insns:
                    o = ins.op1
                    if ((ins.mnemonic == 'jmp' and o and o.type == OpType.REL16)
                            or (ins.mnemonic[0] == 'j' and ins.mnemonic != 'jmp'
                                and o and o.type in (OpType.REL8, OpType.REL16))
                            or (ins.mnemonic.startswith('loop') and o and o.type == OpType.REL8)):
                        t = io + o.disp
                        if not (io <= t < end) and 0 <= t < code_end_img:
                            res["tail"].add(t)
                lifter = X87Lifter(hdr_size=HDR, known_funcs=known)
                lifter.x87 = True          # real x87 ops (needs x87_* in the runtime)
                lifter.dispatch = True
                lifter.far_base = 0
                lifter.jump_tables = jt
                # Video-mode thunk: `xor bh,bh / mov bl,[mode] / shl bx,1 /
                # jmp cs:[bx+table]` -- each driver entry point picks its
                # per-mode routine from a table of near offsets.
                for k, x in enumerate(insns):
                    o = x.op1
                    if (x.mnemonic == 'jmp' and o and o.type == OpType.MEM
                            and o.base == 'bx' and o.seg == 'cs' and x.address not in jt
                            and k == 3):    # the thunk is the whole function (starts at the xor)
                        a3, a2, a1 = insns[k - 3], insns[k - 2], insns[k - 1]
                        if (a3.mnemonic == 'xor' and repr(a3.op1) == 'bh' == repr(a3.op2)
                                and a2.mnemonic == 'mov' and repr(a2.op1) == 'bl'
                                and a2.op2 and a2.op2.type == OpType.MEM
                                and a1.mnemonic == 'shl' and repr(a1.op1) == 'bx'):
                            res["thunks"].add((cs, o.disp))
                # Tables validated here, so another function reading the same
                # (CS base, disp) can reuse them; and the switches that failed.
                by_addr = {x.address: x for x in insns}
                for addr, arms in jt.items():
                    d = by_addr[addr].op1.disp
                    res["tables"].setdefault(d, {})[jbases[addr]] = arms
                for x in insns:
                    o = x.op1
                    if (x.mnemonic == 'jmp' and o and o.type == OpType.MEM
                            and o.base == 'bx' and o.seg == 'cs' and x.address not in jt):
                        res["unres"].append((io, end, cs, o.disp))
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
    forced_total, ignored, dbg_seen = 0, set(), set()
    # Dispatch-miss feedback: addresses the runtime failed to dispatch (written
    # by nep to work/nep_misses.txt) are real code the static pass never named.
    miss_file = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                             "work", "nep_misses_vga.txt" if VARIANT else "nep_misses.txt")
    fed = set()
    if os.path.exists(miss_file):
        for ln in open(miss_file):
            ln = ln.strip()
            if ln:
                t = int(ln, 16)
                if 0 < t < code_end_img and t not in known:
                    fed.add(t)
        print(f"  dispatch-miss feedback: {len(fed)} runtime-discovered starts from {os.path.basename(miss_file)}")
    for rnd in range(6):
        propagate_cs()
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
        for t in res["tail"]:
            if t not in known:
                new.add(t)
        for t in vec_tgts:
            if t not in known:
                new.add(t)
        for t in fed:
            if t not in known:
                new.add(t)
        # Video-mode thunks: tables of one word per mode (0 = unsupported),
        # laid out back to back. A thunk's table ends where the next thunk's
        # (same CS) begins; the last one gets the typical length.
        by_cs = {}
        for cs_, d in res["thunks"]:
            by_cs.setdefault(cs_, set()).add(d)
        n_thunk_arms = 0
        for cs_, ds in by_cs.items():
            ds = sorted(ds)
            gaps = [(b - a) // 2 for a, b in zip(ds, ds[1:]) if 0 < (b - a) // 2 <= 16]
            typical = max(set(gaps), key=gaps.count) if gaps else 5
            for k, d in enumerate(ds):
                n = (ds[k + 1] - d) // 2 if k + 1 < len(ds) and (ds[k + 1] - d) // 2 <= 16 else typical
                for q in range(n):
                    a = cs_ * 16 + d + 2 * q + HDR
                    w = int.from_bytes(data[a:a + 2], 'little')
                    t = cs_ * 16 + w
                    if os.environ.get("NEP_DEBUG_SWITCH") and (cs_, d, q) not in dbg_seen and w:
                        dbg_seen.add((cs_, d, q))
                        print(f"  THUNK cs={cs_:04X} table disp={d:#x} entry {q}: -> {t:05X}")
                    if w and 0 <= t < code_end_img:
                        n_thunk_arms += 1
                        if t not in known:
                            new.add(t)
        thunk_stats = (len(res["thunks"]), n_thunk_arms)
        # Shared tables: a function that jumps through a table another function
        # already resolved (same CS base and disp -- e.g. a copy of a blitter in
        # the same module) gets its arms as forced starts. If its CS was
        # guessed wrong but exactly one validated base fits, adopt that CS.
        cs_changed, n_shared = False, 0
        for io_, end_, cs_, d in res["unres"]:
            cands = res["tables"].get(d, {})
            base = cs_ * 16
            if base not in cands:
                fit = [b for b in cands if 0 <= io_ - b < 0x10000]
                if len(fit) != 1:
                    continue
                base = fit[0]
                if cs_over.get(io_) != base >> 4:
                    cs_over[io_] = base >> 4
                    cs_changed = True
            for t in cands[base]:
                if 0 <= t < code_end_img:
                    n_shared += 1
                    if t not in known:
                        new.add(t)
        shared_stats = n_shared
        if not new and not cs_changed:
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
    print(f"  shared-table switches: {shared_stats} arms reused from tables resolved elsewhere")
    print(f"  video-mode thunks: {thunk_stats[0]} tables, {thunk_stats[1]} per-mode arms")
    print(f"  native overrides: {', '.join(res['overridden']) or 'none'}")
    print(f"  forced starts total: {forced_total}; ignored out-of-code far targets: {len(ignored)}")

    names = [n for n, _, _ in funcs]
    stubs = sorted(c for c in all_calls if c not in set(names))
    if VARIANT:
        import re
        vf = [(n, io_) for n, io_, _ in funcs if V_LO <= io_ < V_HI and io_ not in OVERRIDES]
        ren = lambda t: re.sub(r"\bfn_([0-9A-F]{5})\b",
                               lambda m: ("fv_" + m[1]) if V_LO <= int(m[1], 16) < V_HI else m[0], t)
        vb = [ren(b) for b in bodies]
        for old in os.listdir(OUT):
            if old.startswith("vga_"):
                os.remove(os.path.join(OUT, old))
        with open(os.path.join(OUT, "vga_000.c"), "w") as f:
            f.write('#include "recomp_all.h"\n')
            for n, _ in vf:
                f.write(f"void {ren(n)}(CPU *cpu);\n")
            f.write("\n" + "\n\n".join(vb) + "\n")
        with open(os.path.join(OUT, "vga_dispatch.c"), "w") as f:
            f.write('#include "recomp_all.h"\n')
            for n, _ in vf:
                f.write(f"void {ren(n)}(CPU *cpu);\n")
            f.write("typedef void (*vfn)(CPU *);\ntypedef struct { unsigned addr; vfn fn; } VEnt;\n")
            f.write(f"static const VEnt g_v[{len(vf)}] = {{\n")
            for n, io_ in sorted(vf, key=lambda t: t[1]):
                f.write(f"  {{0x{io_:05X}, {ren(n)}}},\n")
            f.write("};\n")
            f.write(f"int recomp_dispatch_v(CPU *cpu, unsigned addr) {{\n    int lo = 0, hi = {len(vf) - 1};\n"
                    "    while (lo <= hi) { int mid = (lo + hi) / 2;\n"
                    "        if (g_v[mid].addr == addr) { g_v[mid].fn(cpu); return 1; }\n"
                    "        if (g_v[mid].addr < addr) lo = mid + 1; else hi = mid - 1; }\n    return 0;\n}\n")
        print(f"variant vga: {len(vf)} functions -> {OUT}/vga_*.c")
        return
    for old in os.listdir(OUT):
        if old.startswith("recomp_") and old.endswith((".c", ".h")):
            os.remove(os.path.join(OUT, old))
    for i in range(0, len(bodies), CHUNK):
        with open(os.path.join(OUT, f"recomp_{i // CHUNK:03d}.c"), "w") as f:
            f.write('#include "recomp_all.h"\n\n' + "\n\n".join(bodies[i:i + CHUNK]) + "\n")
    with open(os.path.join(OUT, "recomp_all.h"), "w") as f:
        f.write("#ifndef RECOMP_ALL_H\n#define RECOMP_ALL_H\n")
        f.write('#include "cpu.h"\n#include "runtime16.h"\n#include "x87.h"\n\n')
        for sg in sorted(segs_used):
            f.write(f"#define SEG_{sg:04X} 0x{sg:04X}\n")
        f.write("\n")
        for n in names:
            f.write(f"void {n}(CPU *cpu);\n")
        for st in stubs:                      # unresolved-call stubs (defined in recomp_dispatch.c)
            f.write(f"void {st}(CPU *cpu);\n")
        f.write("\n/* native overrides (implement in the runtime):\n")
        for io_, why in sorted(OVERRIDES.items()):
            f.write(f" *   fn_{io_:05X}: {why}\n")
        f.write(" */\n")
        f.write("\n#endif\n")

    # Dispatch table (indirect call/jmp by address) and stubs for call targets
    # that resolved to nothing. Image-space linear address = seg*16 + off.
    live = sorted((io, n) for n, io, _ in funcs)
    if not os.path.exists(os.path.join(OUT, "vga_dispatch.c")):
        with open(os.path.join(OUT, "vga_dispatch.c"), "w") as f:
            f.write('#include "recomp_all.h"\nint recomp_dispatch_v(CPU *cpu, unsigned addr) { (void)cpu; (void)addr; return 0; }\n')
    with open(os.path.join(OUT, "recomp_dispatch.c"), "w") as f:
        f.write('#include "recomp_all.h"\n#include "portable.h"\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n\n')
        f.write("typedef void (*recomp_fn)(CPU *);\n")
        f.write("typedef struct { unsigned addr; recomp_fn fn; } DispEntry;\n")
        f.write(f"static const DispEntry g_disp[{len(live)}] = {{\n")
        for io_, n in live:
            f.write(f"  {{0x{io_:05X}, {n}}},\n")
        f.write("};\n")
        f.write(f"#define N_DISP {len(live)}\n")
        f.write("""
/* in-range misses are recorded for the next lift round (dispatch feedback) */
#define MAXMISS 4096
static unsigned g_miss[MAXMISS], g_missv[MAXMISS]; static int g_nmiss, g_nmissv;
static int g_trace = -1;
static void note_miss(unsigned addr, int vga) {
    unsigned *m = vga ? g_missv : g_miss; int *n = vga ? &g_nmissv : &g_nmiss;
    for (int i = 0; i < *n; i++) if (m[i] == addr) return;
    if (*n < MAXMISS) m[(*n)++] = addr;
}
static void dump_one(const char *path, unsigned *m, int *n, int vga) {
    FILE *r = fopen(path, "r");
    if (r) { unsigned a; while (fscanf(r, "%x", &a) == 1) note_miss(a, vga); fclose(r); }
    FILE *w = fopen(path, "w"); if (!w) return;
    for (int i = 0; i < *n; i++) fprintf(w, "%05X\\n", m[i]);
    fclose(w);
}
/* path = work/nep_misses.txt; the VGA-driver variant's misses go beside it */
void recomp_dump_misses(const char *path) {
    char v[600]; snprintf(v, sizeof v, "%s", path);
    char *dot = strrchr(v, '.'); if (dot) *dot = 0;
    strcat(v, "_vga.txt");
    dump_one(path, g_miss, &g_nmiss, 0);
    if (g_nmissv) dump_one(v, g_missv, &g_nmissv, 1);
}
/* 1 = dispatched, 0 = no function at that address */
int recomp_dispatch_v(CPU *cpu, unsigned addr);
int recomp_dispatch(CPU *cpu, unsigned seg, unsigned off) {
    unsigned addr = ((seg << 4) + off) & 0xFFFFF;
    /* VGA driver patched in (stub 0 is no longer E9): its own routines */
    if (addr >= 0x94C && addr < 0x1070 && cpu->mem[0x910] != 0xE9 && recomp_dispatch_v(cpu, addr)) return 1;
    int lo = 0, hi = N_DISP - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (g_disp[mid].addr == addr) { g_disp[mid].fn(cpu); return 1; }
        if (g_disp[mid].addr < addr) lo = mid + 1; else hi = mid - 1;
    }
    if (g_trace < 0) g_trace = getenv("NEP_TRACE") != NULL;
    if (addr && off < 0xFF00) note_miss(addr, addr >= 0x94C && addr < 0x1070 && cpu->mem[0x910] != 0xE9);   /* offsets near 0xFFFF are negative-wrapped garbage */
    if (g_trace) {
        fprintf(stderr, "[disp] MISS %04X:%04X (image %05X) ds=%04X; from:\\n", seg, off, addr, cpu->ds);
        void *bt[6]; int n = backtrace(bt, 6); backtrace_symbols_fd(bt + 1, n - 1, 2);
    }
    return 0;
}
/* A miss must drop the return frame the call site pushed. */
void dispatch_near(CPU *cpu, unsigned seg, unsigned off) { if (!recomp_dispatch(cpu, seg, off)) cpu->sp += 2; }
void dispatch_far(CPU *cpu, unsigned seg, unsigned off)  { if (!recomp_dispatch(cpu, seg, off)) cpu->sp += 4; }
""")
        f.write("\n/* call targets the lift could not resolve: log, drop the frame */\n")
        for st in stubs:
            drop = 4 if st.startswith("far_") else 2
            f.write(f'void {st}(CPU *cpu) {{ static int n; if (n++ < 4) fprintf(stderr, "[stub] {st}\\n"); cpu->sp += {drop}; }}\n')

    print(f"\nlifted {lifted}/{len(funcs)} functions, {len(failed)} lift errors, "
          f"{n_wrapped} wrapped near targets, {len(stubs)} unresolved call targets")
    if stubs:
        print("  unresolved call targets (first 12): " + ", ".join(stubs[:12]))
    for n, e in failed[:20]:
        print(f"  FAILED {n}: {e}")
    print(f"output: {OUT}  ({(len(bodies) + CHUNK - 1) // CHUNK} chunks)")


if __name__ == "__main__":
    main()
