#!/usr/bin/env python3
"""M0: machine edge table generator for the unflatten pass.

Decodes the terminal instruction of every flat-lifted block (from the
per-block `.ll.bytes` files + `manifest.txt`) and emits `edges.json`:
one entry per block describing its machine exit(s):

    { "0x128a": { "end": 0x1293, "kind": "jcc",
                  "taken": 0x18d7, "notaken": 0x1293,
                  "lifted": [true, true] } }

Kinds:
  jmp   { "target": N, "lifted": [bool] }
  jcc   { "taken": N, "notaken": N, "lifted": [bool, bool] }
  call  { "target": N|null, "ret": N, "lifted": [bool] }
        (static `call rel32`: target known; `call reg` / PLT slot: null)
  ret   {}                          (no static successor)
  indjmp { "ret": N }               (indirect branch; continuation = ret)
  other { }                         (syscall, ff-other, ...)

The `lifted` flags are computed against the set of `sub_<hex>` functions
present in the linked module, so unlifted targets (X87 DIL/SIL holes,
off-function targets) route to the runtime join in the unflattened IR.

Usage:
  flat_edges.py --manifest qa_blocks/manifest.txt \
                --blocks qa_blocks \
                --module qa_linked3.ll \
                --out edges.json [--check module]

With `--check`, also re-derives the audit's linked-exit verification:
every linked exit tail call in the module must target a lifted block
whose pc matches one of this block's static machine successors.
"""

import argparse
import json
import re
import struct
import sys

TERMINALS = {
    "jmp32": 5,
    "jmp8": 2,
    "jcc32": 6,
    "jcc8": 2,
    "call32": 5,
    "ret": 1,
    "ret16": 3,
    "callm_reg": 2,   # ff /2 modrm
    "jmpm_reg": 2,    # ff /4 modrm
    "callm_abs": 6,   # ff 15 (call m64)
    "jmpm_abs": 6,    # ff 25 (jmp m64)
    "syscall": 2,
    "ff_other": 2,
}


def decode_last_insn(data):
    """Return (kind, disp, ilen) of the last instruction, or None.

    Mirrors the audit script's tail scan: the block's bytes end at the
    terminal, so only tails whose end coincides with len(data) count.
    """
    n = len(data)
    cands = []
    for i in range(n - 1, -1, -1):
        b = data[i]
        if b == 0xE9 and i + 5 == n:
            cands.append((i, ("jmp32", struct.unpack_from("<i", data, i + 1)[0], 5)))
        if b == 0xE8 and i + 5 == n:
            cands.append((i, ("call32", struct.unpack_from("<i", data, i + 1)[0], 5)))
        if b == 0xEB and i + 2 == n:
            cands.append((i, ("jmp8", struct.unpack_from("<b", data, i + 1)[0], 2)))
        if b == 0xC3 and i + 1 == n:
            cands.append((i, ("ret", 0, 1)))
        if b == 0xC2 and i + 3 == n:
            cands.append((i, ("ret16", 0, 3)))
        if b == 0x0F and i + 6 == n and 0x80 <= data[i + 1] <= 0x8F:
            cands.append((i, ("jcc32", struct.unpack_from("<i", data, i + 2)[0], 6)))
        if 0x70 <= b <= 0x7F and i + 2 == n:
            cands.append((i, ("jcc8", struct.unpack_from("<b", data, i + 1)[0], 2)))
        if b == 0xFF and i + 6 == n and data[i + 1] == 0x15:
            cands.append((i, ("callm_abs", 0, 6)))
        if b == 0xFF and i + 6 == n and data[i + 1] == 0x25:
            cands.append((i, ("jmpm_abs", 0, 6)))
        if b == 0xFF and i + 2 == n:
            reg = (data[i + 1] >> 3) & 7
            if reg == 2:
                cands.append((i, ("callm_reg", 0, 2)))
            elif reg == 4:
                cands.append((i, ("jmpm_reg", 0, 2)))
            else:
                cands.append((i, ("ff_other", 0, 2)))
        if b == 0x0F and i + 2 == n and data[i + 1] == 0xAF:
            cands.append((i, ("syscall", 0, 2)))
    if not cands:
        return None
    cands.sort()
    return cands[0][1]


def load_manifest(path):
    blocks = {}
    with open(path) as f:
        for line in f:
            p = line.split()
            if len(p) >= 2:
                blocks[int(p[0], 16)] = int(p[1], 16)
    return blocks


def load_lifted(module_path):
    src = open(module_path).read()
    return {int(m, 16) for m in re.findall(r"^define ptr @sub_([0-9a-f]+)\(", src, re.M)}


def build_edges(blocks, lifted):
    edges = {}
    bad = []
    kind_count = {}
    for start, end in sorted(blocks.items()):
        if start not in lifted:
            continue  # emit one entry per lifted (module) block
        try:
            with open(f"{args_blocks}/b_{start:05x}.ll.bytes") as f:
                data = bytes.fromhex(f.read().strip())
        except FileNotFoundError:
            bad.append((hex(start), "no bytes file"))
            continue
        li = decode_last_insn(data)
        if li is None:
            bad.append((hex(start), "no terminal opcode"))
            continue
        kind, disp, ilen = li
        e_end = start + len(data)
        if e_end != end:
            bad.append((hex(start), f"manifest end {hex(end)} != start+len {hex(e_end)}"))
            continue

        if kind in ("jmp32", "jmp8"):
            t = (e_end + disp) & 0xFFFFFFFF
            edges[hex(start)] = {
                "end": hex(e_end), "kind": "jmp", "target": hex(t),
                "lifted": [t in lifted],
            }
        elif kind in ("jcc32", "jcc8"):
            t = (e_end + disp) & 0xFFFFFFFF
            f = e_end
            edges[hex(start)] = {
                "end": hex(e_end), "kind": "jcc",
                "taken": hex(t), "notaken": hex(f),
                "lifted": [t in lifted, f in lifted],
            }
        elif kind == "call32":
            t = (e_end + disp) & 0xFFFFFFFF
            edges[hex(start)] = {
                "end": hex(e_end), "kind": "call", "target": hex(t),
                "ret": hex(e_end), "static": True,
                # [continuation lifted, call target lifted]
                "lifted": [e_end in lifted, t in lifted],
            }
        elif kind in ("callm_reg", "callm_abs"):
            edges[hex(start)] = {
                "end": hex(e_end), "kind": "call", "target": None,
                "ret": hex(e_end), "static": False,
                # [continuation lifted, target known? no]
                "lifted": [e_end in lifted, False],
            }
        elif kind in ("ret", "ret16"):
            edges[hex(start)] = {"end": hex(e_end), "kind": "ret"}
        elif kind in ("jmpm_reg", "jmpm_abs"):
            edges[hex(start)] = {"end": hex(e_end), "kind": "indjmp",
                                 "ret": hex(e_end)}
        elif kind == "syscall":
            edges[hex(start)] = {"end": hex(e_end), "kind": "syscall"}
        else:
            edges[hex(start)] = {"end": hex(e_end), "kind": "ff_other"}
        kind_count[edges[hex(start)]["kind"]] = \
            kind_count.get(edges[hex(start)]["kind"], 0) + 1
    return edges, bad, kind_count


args_blocks = None


def audit_check(module_path, edges, blocks):
    """Re-derive the audit's 369/369: every linked exit tail call goes to
    a lifted block whose pc is a static successor of the caller block."""
    src = open(module_path).read()
    funcs = re.findall(r"^define ptr @sub_([0-9a-f]+)\(.*?^}", src, re.M | re.S)
    body_re = re.compile(
        r"tail call ptr @sub_([0-9a-f]+)\(", re.M)
    n_link = 0
    n_bad = 0
    for line in src.splitlines():
        m = re.search(r"tail call ptr @sub_([0-9a-f]+)\(", line)
        if not m:
            continue
        n_link += 1
    # Per-function check: find the caller block for each call line.
    line_no = 0
    cur_func = None
    for line in src.splitlines():
        m = re.match(r"^define ptr @sub_([0-9a-f]+)\(", line)
        if m:
            cur_func = int(m.group(1), 16)
            continue
        if line.startswith("}"):
            cur_func = None
            continue
        m = re.search(r"tail call ptr @sub_([0-9a-f]+)\(", line)
        if m and cur_func is not None:
            tgt = int(m.group(1), 16)
            e = edges.get(hex(cur_func))
            if e is None:
                n_bad += 1
                continue
            ok = False
            if e["kind"] in ("jmp", "call"):
                ok = e.get("target") == hex(tgt) or (
                    e["kind"] == "call" and e.get("ret") == hex(tgt))
            elif e["kind"] == "jcc":
                ok = e["taken"] == hex(tgt) or e["notaken"] == hex(tgt)
            if not ok:
                n_bad += 1
                print(f"  BAD: sub_{cur_func:x} -> sub_{tgt:x} "
                      f"(expected {e})")
    return n_link, n_bad


def main():
    global args_blocks
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--blocks", required=True, help="dir with b_<hex>.ll.bytes")
    ap.add_argument("--module", required=True, help="linked module .ll")
    ap.add_argument("--out", required=True)
    ap.add_argument("--check", action="store_true",
                    help="re-derive the audit's linked-exit verification")
    a = ap.parse_args()
    args_blocks = a.blocks

    blocks = load_manifest(a.manifest)
    lifted = load_lifted(a.module)
    edges, bad, kind_count = build_edges(blocks, lifted)

    with open(a.out, "w") as f:
        json.dump(edges, f, indent=1, sort_keys=True)

    print(f"blocks in manifest: {len(blocks)}")
    print(f"edges emitted:      {len(edges)}")
    print(f"lifted blocks:      {len(lifted)}")
    print(f"kind counts:        {json.dumps(kind_count)}")
    def has_lifted_successor(e):
        if e["kind"] == "jmp":
            return e["lifted"][0]
        if e["kind"] == "jcc":
            return any(e["lifted"])
        if e["kind"] == "call":
            return e["lifted"][0]  # continuation (return address)
        return False

    n_lift_static = sum(1 for e in edges.values() if has_lifted_successor(e))
    print(f"blocks with >=1 lifted static successor: {n_lift_static}")
    for b in bad:
        print("  BAD:", b)

    if a.check:
        n_link, n_bad = audit_check(a.module, edges, blocks)
        print(f"linked exit calls in module: {n_link}, BAD: {n_bad}")


if __name__ == "__main__":
    main()
